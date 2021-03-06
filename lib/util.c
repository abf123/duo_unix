/*
 * util.c
 *
 * Copyright (c) 2013 Duo Security
 * All rights reserved, all wrongs reversed
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <utime.h>
#include <unistd.h>

#include "util.h"
#include "groupaccess.h"

int duo_debug = 0;

void
duo_config_default(struct duo_config *cfg)
{
    memset(cfg, 0, sizeof(struct duo_config));
    cfg->failmode = DUO_FAIL_SAFE;
    cfg->prompts = MAX_PROMPTS;
    cfg->local_ip_fallback = 0;
    cfg->https_timeout = -1;
    cfg->ta_expire = 0;
    cfg->ta_prefix = "";
}

int
duo_set_boolean_option(const char *val)
{
    if (strcmp(val, "yes") == 0 || strcmp(val, "true") == 0 ||
        strcmp(val, "on") == 0 || strcmp(val, "1") == 0) {
        return (1);
    } else {
        return (0);
    }
}

int
duo_common_ini_handler(struct duo_config *cfg, const char *section, 
    const char *name, const char*val) 
{
    char *buf, *p;
    int int_val;
    
    if (strcmp(name, "ikey") == 0) {
        cfg->ikey = strdup(val);
    } else if (strcmp(name, "skey") == 0) {
        cfg->skey = strdup(val);
    } else if (strcmp(name, "host") == 0) {
        cfg->apihost = strdup(val);
    } else if (strcmp(name, "cafile") == 0) {
        cfg->cafile = strdup(val);
    } else if (strcmp(name, "http_proxy") == 0) {
        cfg->http_proxy = strdup(val);
    } else if (strcmp(name, "groups") == 0 || strcmp(name, "group") == 0) {
        if ((buf = strdup(val)) == NULL) {
            fprintf(stderr, "Out of memory parsing groups\n");
            return (0);
        }
        for (p = strtok(buf, " "); p != NULL; p = strtok(NULL, " ")) {
            if (cfg->groups_cnt >= MAX_GROUPS) {
                fprintf(stderr, "Exceeded max %d groups\n",
                    MAX_GROUPS);
                cfg->groups_cnt = 0;
                free(buf);
                return (0);
            }
            cfg->groups[cfg->groups_cnt++] = p;
        }
    } else if (strcmp(name, "failmode") == 0) {
        if (strcmp(val, "secure") == 0) {
            cfg->failmode = DUO_FAIL_SECURE;
        } else if (strcmp(val, "safe") == 0) {
            cfg->failmode = DUO_FAIL_SAFE;
        } else {
            fprintf(stderr, "Invalid failmode: '%s'\n", val);
            return (0);
        }
    } else if (strcmp(name, "pushinfo") == 0) {
        cfg->pushinfo = duo_set_boolean_option(val);
    } else if (strcmp(name, "noverify") == 0) {
        cfg->noverify = duo_set_boolean_option(val);
    } else if (strcmp(name, "prompts") == 0) {
        int_val = atoi(val);
        /* Clamp the value into acceptable range */
        if (int_val <= 0) {
            int_val = 1;
        } else if (int_val < cfg->prompts) {
            cfg->prompts = int_val;
        }
    } else if (strcmp(name, "autopush") == 0) {
        cfg->autopush = duo_set_boolean_option(val);
    } else if (strcmp(name, "accept_env_factor") == 0) {
        cfg->accept_env = duo_set_boolean_option(val);
    } else if (strcmp(name, "fallback_local_ip") == 0) {
        cfg->local_ip_fallback = duo_set_boolean_option(val);
    } else if (strcmp(name, "https_timeout") == 0) {
        cfg->https_timeout = atoi(val);
        if (cfg->https_timeout <= 0) {
            cfg->https_timeout = -1; /* no timeout */
        }
        else {
            /* Make timeout milliseconds */
            cfg->https_timeout *= 1000;
        }
    } else if (strcmp(name, "taexpire") == 0) {
        int_val = atoi(val);
        /* Clamp the value into acceptable range */
        if (int_val <= 0) {
            cfg->ta_expire = 0;
        } else if (int_val > MAX_TA_EXPIRE) {
            cfg->ta_expire = MAX_TA_EXPIRE;
        } else {
            cfg->ta_expire = int_val;
        }
    } else if (strcmp(name, "taprefix") == 0) {
        cfg->ta_prefix = strdup(val);
    } else if (strcmp(name, "send_gecos") == 0) {
      cfg->send_gecos = duo_set_boolean_option(val);
    } else {
        /* Couldn't handle the option, maybe it's target specific? */
        return (0);
    }
    return (1);
}

int 
duo_check_trusted_access(struct passwd *pw, struct duo_config *cfg, const char *ip)
{
    int trusted = 0;
    struct stat sb;
    time_t now;
    const char * filename;

    /* Get current time */
    time(&now);
    
    /* Get filename */
    filename = duo_trusted_access_filename(pw, cfg, ip);

    /* Check if a trusted access file exists (can be stat'ed) */
    if (stat(filename, &sb) == -1) {
        /* No trusted file exists, untrusted */
        trusted = 0;
    } else if (difftime(now,sb.st_mtime)<(double)(cfg->ta_expire*60.0)) {
        /* Trusted file exists and is within expiration */
        trusted = 1;
        /* Reset file modified time */
        duo_touch_trusted_access_file(filename);
    } else {
        /* Trusted file exists but has expired */
        trusted = 0;
    }

    return trusted;
}

char *
duo_trusted_access_filename(struct passwd *pw, struct duo_config *cfg, const char *ip)
{
    int ret;
    char * ta_filename;
    char * ta_prefix;

    /* Handle test cases where there is no ip */
    if (ip == NULL) {
        ip = "localhost";
    }
    
    /* Honor trusted access file prefix or use home as a default */
    if (strlen(cfg->ta_prefix) > 0) {
        ta_prefix = strdup(cfg->ta_prefix);
    } else {
        ta_prefix = strdup(pw->pw_dir);
   }
    
    /* Construct filename */
    ret = asprintf(&ta_filename, "%s/.ds-%s-%s", ta_prefix, pw->pw_name, ip);
    
    if (ret>=0) {
        return ta_filename;
    } else {
        return NULL;
    }
}

void
duo_touch_trusted_access_file(const char *ta_filename)
{
    FILE * fd;
    struct utimbuf ubuf;

    /* Set access and mod time to current time */
    time(&ubuf.actime);
    time(&ubuf.modtime);

    /* "Touch" the file */
    if ((fd = fopen(ta_filename,"w")) != NULL) {
        fclose(fd);
        if (utime(ta_filename, &ubuf) != 0) {
            duo_log(LOG_ERR, "Couldn't write cached access file mod time",
                ta_filename, NULL, NULL);
        }
    } else {
        duo_log(LOG_ERR, "Couldn't write cached access file",
            ta_filename, NULL, NULL);
    }

    return;
}

int 
duo_check_groups(struct passwd *pw, char **groups, int groups_cnt)
{
    int i;

    if (groups_cnt > 0) {
        int matched = 0;
        
        if (ga_init(pw->pw_name, pw->pw_gid) < 0) {
            duo_log(LOG_ERR, "Couldn't get groups",
                pw->pw_name, NULL, strerror(errno));
            return (-1);
        }
        for (i = 0; i < groups_cnt; i++) {
            if (ga_match_pattern_list(groups[i])) {
                matched = 1;
                break;
            }
        }
        ga_free();
        
        /* User in configured groups for Duo auth? */
        return matched;
    } else {
        return 1;
    }
}

void
duo_log(int priority, const char*msg, const char *user, const char *ip,
        const char *err) 
{
    char buf[512];
    int i, n;

    n = snprintf(buf, sizeof(buf), "%s", msg);

    if (user != NULL &&
        (i = snprintf(buf + n, sizeof(buf) - n, " for '%s'", user)) > 0) {
        n += i;
    }
    if (ip != NULL &&
        (i = snprintf(buf + n, sizeof(buf) - n, " from %s", ip)) > 0) {
        n += i;
    }
    if (err != NULL &&
        (i = snprintf(buf + n, sizeof(buf) - n, ": %s", err)) > 0) {
        n += i;
    }
    duo_syslog(priority, "%s", buf);
}

void
duo_syslog(int priority, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    if (duo_debug) {
        fprintf(stderr, "[%d] ", priority);
        vfprintf(stderr, fmt, ap);
        fputs("\n", stderr);
    } else {
        vsyslog(priority, fmt, ap);
    }
    va_end(ap);
}

const char *
duo_local_ip()
{
    struct sockaddr_in sin;
    socklen_t slen;
    int fd;
    const char *ip = NULL;

    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = inet_addr("8.8.8.8"); /* XXX Google's DNS Server */
    sin.sin_port = htons(53);
    slen = sizeof(sin);

    if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) != -1) {
            if (connect(fd, (struct sockaddr *)&sin, slen) != -1 &&
                getsockname(fd, (struct sockaddr *)&sin, &slen) != -1) {
                    ip = inet_ntoa(sin.sin_addr); /* XXX statically allocated */
            }
            close(fd);
    }
    return (ip);
}


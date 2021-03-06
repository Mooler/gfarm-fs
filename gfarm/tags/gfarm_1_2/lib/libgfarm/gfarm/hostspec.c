/*
 * $Id$
 */

#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <gfarm/gfarm_error.h>
#include <gfarm/gfarm_misc.h> /* gfarm_host_is_in_domain() */
#include "hostspec.h"

#define IS_DNS_LABEL_CHAR(c)	(isalnum(c) || (c) == '-')
#define AF_INET4_BIT	32

struct gfarm_hostspec {
	enum { GFHS_NAME, GFHS_AF_INET4 } type;
	union gfhs_union {
		char name[1];
		struct gfhs_in4_addr {
			struct in_addr addr, mask;
		} in4_addr;
	} u;
};

char *
gfarm_hostspec_name_new(char *name, struct gfarm_hostspec **hostspecpp)
{
	struct gfarm_hostspec *hsp = malloc(sizeof(struct gfarm_hostspec)
	    - sizeof(union gfhs_union) + strlen(name) + 1);

	if (hsp == NULL)
		return (GFARM_ERR_NO_MEMORY);
	hsp->type = GFHS_NAME;
	strcpy(hsp->u.name, name);
	*hostspecpp = hsp;
	return (NULL);
}

char *
gfarm_hostspec_af_inet4_new(gfarm_int32_t addr, gfarm_int32_t mask,
	struct gfarm_hostspec **hostspecpp)
{
	struct gfarm_hostspec *hsp = malloc(sizeof(struct gfarm_hostspec)
	    - sizeof(union gfhs_union) + sizeof(struct gfhs_in4_addr));

	if (hsp == NULL)
		return (GFARM_ERR_NO_MEMORY);
	hsp->type = GFHS_AF_INET4;
	hsp->u.in4_addr.addr.s_addr = addr;
	hsp->u.in4_addr.mask.s_addr = mask;
	*hostspecpp = hsp;
	return (NULL);
}

void
gfarm_hostspec_free(struct gfarm_hostspec *hostspecp)
{
	free(hostspecp);
}

/*
 * We don't use inet_addr(3)/inet_aton(3)/inet_pton(3),
 * because these library functions permit not only a.b.c.d,
 * but also a.b.c, a.b and a as inet address.
 *
 * NOTE: this function stores address of first invalid charactor to *endptr,
 *	even if this function reports error as its return value.
 */
char *
gfarm_string_to_in4addr(char *s, char **endptr, struct in_addr *addrp)
{
	char *e, *ep;
	gfarm_int32_t addr;
	unsigned long byte;
	int i;
	static char invalid_char[] = "invalid character in IP address";
	static char too_big_byte[] = "too big byte value in IP address";

	byte = strtoul(s, &ep, 10);
	if (ep == s) {
		e = *s == '\0' ? "IP address expected" : invalid_char;
		goto bad;
	}
	if (byte >= 256) {
		ep = s;
		e = too_big_byte;
		goto bad;
	}
	addr = byte;
	for (i = 0; i < 3; i++) {
		if (*ep != '.') {
			e = "IP address less than 4 bytes";
			goto bad;
		}
		s = ep + 1;
		byte = strtoul(s, &ep, 10);
		if (ep == s) {
			e = invalid_char;
			goto bad;
		}
		if (byte >= 256) {
			ep = s;
			e = too_big_byte;
			goto bad;
		}
		addr = (addr << 8) | byte;
	}
	addrp->s_addr = htonl(addr);
	if (endptr != NULL)
		*endptr = ep;
	return (NULL);
bad:
	if (endptr != NULL)
		*endptr = ep;
	return (e);
}

static int
gfarm_is_string_upper_case(char *s)
{
	unsigned char *t = (unsigned char *)s;

	for (; *t != '\0'; t++) {
		if (!isupper(*t))
			return (0);
	}
	return (1);
}

char *
gfarm_hostspec_parse(char *name, struct gfarm_hostspec **hostspecpp)
{
	char *end1p, *end2p;
	struct in_addr addr, mask;
	unsigned long masklen;

	if (strcmp(name, "*") == 0 || strcmp(name, "ALL") == 0) {
		return (gfarm_hostspec_af_inet4_new(INADDR_ANY, INADDR_ANY,
		    hostspecpp));
	}
	if (gfarm_string_to_in4addr(name, &end1p, &addr) == NULL) {
		if (*end1p == '\0') {
			return (gfarm_hostspec_af_inet4_new(
			    addr.s_addr, INADDR_BROADCAST, hostspecpp));
		}
		if (*end1p == '/') {
			if (isdigit(((unsigned char *)end1p)[1]) &&
			    (masklen = strtoul(end1p + 1, &end2p, 10),
			     *end2p == '\0')) {
				if (masklen > AF_INET4_BIT)
					return ("too big netmask");
				if (masklen == 0) {
					mask.s_addr = INADDR_ANY;
				} else {
					masklen = AF_INET4_BIT - masklen;
					mask.s_addr = htonl(
					    ~((1 << (gfarm_int32_t)masklen)
					      - 1));
				}
				return (gfarm_hostspec_af_inet4_new(
				    addr.s_addr, mask.s_addr, hostspecpp));
			} else if (gfarm_string_to_in4addr(end1p + 1,
			    &end2p, &mask) == NULL && *end2p == '\0') {
				return (gfarm_hostspec_af_inet4_new(
				    addr.s_addr, mask.s_addr,
				    hostspecpp));
			}
		}
		if (!IS_DNS_LABEL_CHAR(*(unsigned char *)end1p) &&
		    *end1p != '.')
			return ("invalid character in IP address");
	}
	if (*name == '\0')
		return ("hostname or IP address expected");
	if (!IS_DNS_LABEL_CHAR(*(unsigned char *)end1p) && *end1p != '.')
		return ("invalid character in hostname");

	/*
	 * We don't allow all capital domain name.
	 * Such names are reserved for keywords like "*", "LISTENER".
	 */
	if (gfarm_is_string_upper_case(name))
		return ("unknown keyword");

	return (gfarm_hostspec_name_new(name, hostspecpp));
}

int
gfarm_host_is_in_domain(const char *hostname, const char *domainname)
{
	int hlen = strlen(hostname), dlen = strlen(domainname);

	if (hlen < dlen)
		return (0);
	if (hlen == dlen)
		return (strcasecmp(hostname, domainname) == 0);
	if (dlen == 0)
		return (1); /* null string matches with all hosts */
	if (hlen == dlen + 1)
		return (0);
	return (hostname[hlen - (dlen + 1)] == '.' &&
	    strcasecmp(&hostname[hlen - dlen], domainname) == 0);
}

int
gfarm_hostspec_match(struct gfarm_hostspec *hostspecp,
	const char *name, struct sockaddr *addr)
{
	switch (hostspecp->type) {
	case GFHS_NAME:
		if (name == NULL)
			return (0);
		if (hostspecp->u.name[0] == '.') {
			return (gfarm_host_is_in_domain(name, 
			    &hostspecp->u.name[1]));
		} else {
			return (strcasecmp(name, hostspecp->u.name) == 0);
		}
	case GFHS_AF_INET4:
		if (addr == NULL || addr->sa_family != AF_INET)
			return (0);
		return ((((struct sockaddr_in *)addr)->sin_addr.s_addr &
			 hostspecp->u.in4_addr.mask.s_addr) ==
			hostspecp->u.in4_addr.addr.s_addr);
	}
	/* assert(0); */
	return (0);
}

char *
gfarm_sockaddr_to_name(struct sockaddr *addr, char **namep)
{
	int i, addrlen, addrfamily = addr->sa_family;
	char *addrp, *name;
	struct hostent *hp;

	switch (addrfamily) {
	case AF_INET:
		addrp = (char *)&((struct sockaddr_in *)addr)->sin_addr;
		addrlen = sizeof(struct in_addr);
		break;
	default:
		return (gfarm_errno_to_error(EAFNOSUPPORT));
	}
	hp = gethostbyaddr(addrp, addrlen, addrfamily);
	if (hp == NULL)
		return ("hostname is not provided");
	name = strdup(hp->h_name);
	if (name == NULL)
		return (GFARM_ERR_NO_MEMORY);
#ifdef HAVE_GETHOSTBYNAME2
	hp = gethostbyname2(name, addrfamily);
#else
	hp = gethostbyname(name);
#endif
	if (hp == NULL) {
		free(name);
		return ("reverse-lookup-name isn't resolvable");
	}
	if (hp->h_addrtype != addrfamily || hp->h_length != addrlen) {
		free(name);
		return ("invalid name record");
	}
	for (i = 0; hp->h_addr_list[i] != NULL; i++) {
		if (memcmp(hp->h_addr_list[i], addrp, addrlen) == 0) {
			*namep = name;
			return (NULL); /* success */
		}
	}
	free(name);
	return ("reverse-lookup-name doesn't match, possibly forged");
}

void
gfarm_sockaddr_to_string(struct sockaddr *addr, char *string, size_t size)
{
	unsigned char *a;
#ifndef HAVE_SNPRINTF
	char s[GFARM_SOCKADDR_STRLEN];

	if (size <= 0)
		return;
#endif
	switch (addr->sa_family) {
	case AF_INET:
		a = (unsigned char *)
		    &((struct sockaddr_in *)addr)->sin_addr.s_addr;
#ifdef HAVE_SNPRINTF
		snprintf(string, size, "%d.%d.%d.%d", a[0], a[1], a[2], a[3]);
#else
		sprintf(s, "%d.%d.%d.%d", a[0], a[1], a[2], a[3]);
		strncpy(string, s, size);
		string[size - 1] = '\0';
#endif
		break;
	default:
#ifdef HAVE_SNPRINTF
		snprintf(string, size, "unknown address family %d",
			 addr->sa_family);
#else
		sprintf(s, "unknown address family %d", addr->sa_family);
		strncpy(string, s, size);
		string[size - 1] = '\0';
#endif
		break;
	}
}

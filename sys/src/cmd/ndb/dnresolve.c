/*
 * domain name resolvers, see rfcs 1035 and 1123
 */
#include <u.h>
#include <libc.h>
#include <ip.h>
#include <bio.h>
#include <ndb.h>
#include "dns.h"

#define desttrace(...)	do if(0)print(__VA_ARGS__); else USED(__VA_ARGS__); while(0)

typedef struct Dest Dest;
typedef struct Ipaddr Ipaddr;
typedef struct Query Query;

enum
{
	Udp, Udpedns0, Tcp,

	Answrequeryerr=	-2,
	Answerr=	-1,
	Answnone,

	Maxdest=	24,	/* maximum destinations for a request message */
	Maxoutstanding=	15,	/* max. outstanding queries per domain name */
	Remntretry=	15,	/* min. sec.s between /net.alt remount tries */

	/*
	 * these are the old values; we're trying longer timeouts now
	 * primarily for the benefit of remote nameservers querying us
	 * during times of bad connectivity.
	 */
//	Maxtrans=	3,	/* maximum transmissions to a server */
//	Maxretries=	3, /* cname+actual resends: was 32; have pity on user */
//	Maxwaitms=	1000,	/* wait no longer for a remote dns query */
//	Minwaitms=	100,	/* willing to wait for a remote dns query */

	Maxtrans=	5,	/* maximum transmissions to a server */
	Maxretries=	5, /* cname+actual resends: was 32; have pity on user */
	Maxwaitms=	5000,	/* wait no longer for a remote dns query */
	Minwaitms=	500,	/* willing to wait for a remote dns query */

	Destmagic=	0xcafebabe,
	Querymagic=	0xdeadbeef,
};
enum { Hurry, Patient, };
enum { Outns, Inns, };

struct Ipaddr {
	Ipaddr *next;
	uchar	ip[IPaddrlen];
};

struct Dest
{
	uchar	a[IPaddrlen];	/* ip address */
	DN	*s;		/* name server */
	int	nx;		/* number of transmissions */
	int	code;		/* response code; used to clear dp->respcode */

	ulong	magic;
};

/*
 * Query has a QLock in it, thus it can't be an automatic
 * variable, since each process would see a separate copy
 * of the lock on its stack.
 */
struct Query {
	DN	*dp;		/* domain */
	ushort	type;		/* and type to look up */
	Request *req;
	RR	*nsrp;		/* name servers to consult */

	/* dest must not be on the stack due to forking in slave() */
	Dest	*dest;		/* array of destinations */
	Dest	*curdest;	/* pointer to next to fill */
	int	ndest;		/* transmit to this many on this round */

	int	udpfd;

	QLock	tcplock;	/* only one tcp call at a time per query */
	int	tcpset;
	int	tcpfd;		/* if Tcp, read replies from here */
	int	tcpctlfd;
	uchar	tcpip[IPaddrlen];

	ulong	magic;
};

/* estimated % probability of such a record existing at all */
int likely[] = {
	[Ta]		95,
	[Taaaa]		10,
	[Tcname]	15,
	[Tmx]		60,
	[Tns]		90,
	[Tnull]		5,
	[Tptr]		35,
	[Tsoa]		90,
	[Tsrv]		60,
	[Ttxt]		15,
	[Tspf]		15,
	[Tall]		95,
};

static char *mediumstr[] = {
[Udp]		"udp",
[Udpedns0]	"edns0",
[Tcp]		"tcp",
};

static RR*	dnresolve1(char*, int, int, Request*, int, int);
static int	netquery(Query *, int);

/*
 * reading /proc/pid/args yields either "name args" or "name [display args]",
 * so return only display args, if any.
 */
static char *
procgetname(void)
{
	int fd, n;
	char *lp, *rp;
	char buf[256];

	snprint(buf, sizeof buf, "#p/%d/args", getpid());
	if((fd = open(buf, OREAD)) < 0)
		return strdup("");
	*buf = '\0';
	n = read(fd, buf, sizeof buf-1);
	close(fd);
	if (n >= 0)
		buf[n] = '\0';
	if ((lp = strchr(buf, '[')) == nil ||
	    (rp = strrchr(buf, ']')) == nil)
		return strdup("");
	*rp = '\0';
	return strdup(lp+1);
}

void
rrfreelistptr(RR **rpp)
{
	RR *rp;

	if (rpp == nil || *rpp == nil)
		return;
	rp = *rpp;
	*rpp = nil;	/* update pointer in memory before freeing list */
	rrfreelist(rp);
}

/*
 *  lookup 'type' info for domain name 'name'.  If it doesn't exist, try
 *  looking it up as a canonical name.
 *
 *  this process can be quite slow if time-outs are set too high when querying
 *  nameservers that just don't respond to certain query types.  in that case,
 *  there will be multiple udp retries, multiple nameservers will be queried,
 *  and this will be repeated for a cname query.  the whole thing will be
 *  retried several times until we get an answer or a time-out.
 */
RR*
dnresolve(char *name, int class, int type, Request *req, RR **cn, int depth,
	int recurse, int rooted, int *status)
{
	RR *rp, *nrp, *drp;
	DN *dp;
	int loops;
	char *procname;
	char nname[Domlen];

	if(status)
		*status = 0;

	if(depth > 12)			/* in a recursive loop? */
		return nil;

	procname = procgetname();
	/*
	 *  hack for systems that don't have resolve search
	 *  lists.  Just look up the simple name in the database.
	 */
	if(!rooted && strchr(name, '.') == nil){
		rp = nil;
		drp = domainlist(class);
		for(nrp = drp; rp == nil && nrp != nil; nrp = nrp->next){
			snprint(nname, sizeof nname, "%s.%s", name,
				nrp->ptr->name);
			rp = dnresolve(nname, class, type, req, cn, depth+1,
				recurse, rooted, status);
			lock(&dnlock);
			rrfreelist(rrremneg(&rp));
			unlock(&dnlock);
		}
		if(drp != nil)
			rrfreelist(drp);
		procsetname(procname);
		free(procname);
		return rp;
	}

	/*
	 *  try the name directly
	 */
	rp = dnresolve1(name, class, type, req, depth, recurse);
	if(rp == nil) {
		/*
		 * try it as a canonical name if we weren't told
		 * that the name didn't exist
		 */
		dp = dnlookup(name, class, 0);
		if(type != Tptr && dp->respcode != Rname)
			for(loops = 0; rp == nil && loops < Maxretries; loops++){
				/* retry cname, then the actual type */
				rp = dnresolve1(name, class, Tcname, req,
					depth, recurse);
				if(rp == nil)
					break;

				/* rp->host == nil shouldn't happen, but does */
				if(rp->negative || rp->host == nil){
					rrfreelist(rp);
					rp = nil;
					break;
				}

				name = rp->host->name;
				lock(&dnlock);
				if(cn)
					rrcat(cn, rp);
				else
					rrfreelist(rp);
				unlock(&dnlock);

				rp = dnresolve1(name, class, type, req,
					depth, recurse);
			}

		/* distinction between not found and not good */
		if(rp == nil && status != nil && dp->respcode != Rok)
			*status = dp->respcode;
	}
	procsetname(procname);
	free(procname);
	return randomize(rp);
}

static void
queryinit(Query *qp, DN *dp, int type, Request *req)
{
	memset(qp, 0, sizeof *qp);
	qp->udpfd = qp->tcpfd = qp->tcpctlfd = -1;
	qp->dp = dp;
	qp->type = type;
	if (qp->type != type)
		dnslog("queryinit: bogus type %d", type);
	qp->req = req;
	qp->nsrp = nil;
	qp->dest = qp->curdest = nil;
	qp->magic = Querymagic;
}

static void
queryck(Query *qp)
{
	assert(qp);
	assert(qp->magic == Querymagic);
}

static void
querydestroy(Query *qp)
{
	queryck(qp);
	/* leave udpfd open */
	if (qp->tcpfd > 0)
		close(qp->tcpfd);
	if (qp->tcpctlfd > 0) {
		hangup(qp->tcpctlfd);
		close(qp->tcpctlfd);
	}
	free(qp->dest);
	memset(qp, 0, sizeof *qp);	/* prevent accidents */
	qp->udpfd = qp->tcpfd = qp->tcpctlfd = -1;
}

static void
destinit(Dest *p)
{
	memset(p, 0, sizeof *p);
	p->magic = Destmagic;
}

static void
destck(Dest *p)
{
	assert(p);
	assert(p->magic == Destmagic);
}

/*
 * if the response to a query hasn't arrived within 100 ms.,
 * it's unlikely to arrive at all.  after 1 s., it's really unlikely.
 * queries for missing RRs are likely to produce time-outs rather than
 * negative responses, so cname and aaaa queries are likely to time out,
 * thus we don't wait very long for them.
 */
static void
notestats(vlong start, int tmout, int type)
{
	qlock(&stats);
	if (tmout) {
		stats.tmout++;
		if (type == Taaaa)
			stats.tmoutv6++;
		else if (type == Tcname)
			stats.tmoutcname++;
	} else {
		long wait10ths = NS2MS(nsec() - start) / 100;

		if (wait10ths <= 0)
			stats.under10ths[0]++;
		else if (wait10ths >= nelem(stats.under10ths))
			stats.under10ths[nelem(stats.under10ths) - 1]++;
		else
			stats.under10ths[wait10ths]++;
	}
	qunlock(&stats);
}

static void
noteinmem(void)
{
	qlock(&stats);
	stats.answinmem++;
	qunlock(&stats);
}

/* netquery with given name servers, free ns rrs when done */
static int
netqueryns(Query *qp, int depth, RR *nsrp)
{
	int rv;

	qp->nsrp = nsrp;
	rv = netquery(qp, depth);
	lock(&dnlock);
	rrfreelist(nsrp);
	unlock(&dnlock);
	return rv;
}

static RR*
issuequery(Query *qp, char *name, int class, int depth, int recurse)
{
	char *cp;
	DN *nsdp;
	RR *rp, *nsrp, *dbnsrp;

	/*
	 *  if we're running as just a resolver, query our
	 *  designated name servers
	 */
	if(cfg.resolver){
		nsrp = randomize(getdnsservers(class));
		if(nsrp != nil)
			if(netqueryns(qp, depth+1, nsrp) > Answnone)
				return rrlookup(qp->dp, qp->type, OKneg);
	}

	/*
 	 *  walk up the domain name looking for
	 *  a name server for the domain.
	 */
	for(cp = name; cp; cp = walkup(cp)){
		/*
		 *  if this is a local (served by us) domain,
		 *  return answer
		 */
		dbnsrp = randomize(dblookup(cp, class, Tns, 0, 0));
		if(dbnsrp && dbnsrp->local){
			rp = dblookup(name, class, qp->type, 1, dbnsrp->ttl);
			lock(&dnlock);
			rrfreelist(dbnsrp);
			unlock(&dnlock);
			return rp;
		}

		/*
		 *  if recursion isn't set, just accept local
		 *  entries
		 */
		if(recurse == Dontrecurse){
			if(dbnsrp) {
				lock(&dnlock);
				rrfreelist(dbnsrp);
				unlock(&dnlock);
			}
			continue;
		}

		/* look for ns in cache */
		nsdp = dnlookup(cp, class, 0);
		nsrp = nil;
		if(nsdp)
			nsrp = randomize(rrlookup(nsdp, Tns, NOneg));

		/* if the entry timed out, ignore it */
		if(nsrp && nsrp->ttl < now){
			lock(&dnlock);
			rrfreelistptr(&nsrp);
			unlock(&dnlock);
		}

		if(nsrp){
			lock(&dnlock);
			rrfreelistptr(&dbnsrp);
			unlock(&dnlock);

			/* query the name servers found in cache */
			if(netqueryns(qp, depth+1, nsrp) > Answnone)
				return rrlookup(qp->dp, qp->type, OKneg);
		} else if(dbnsrp)
			/* try the name servers found in db */
			if(netqueryns(qp, depth+1, dbnsrp) > Answnone)
				return rrlookup(qp->dp, qp->type, NOneg);
	}
	return nil;
}

static RR*
dnresolve1(char *name, int class, int type, Request *req, int depth,
	int recurse)
{
	Area *area;
	DN *dp;
	RR *rp;
	Query *qp;

	if(debug)
		dnslog("[%d] dnresolve1 %s %d %d", getpid(), name, type, class);

	/* only class Cin implemented so far */
	if(class != Cin)
		return nil;

	dp = dnlookup(name, class, 1);

	/*
	 *  Try the cache first
	 */
	rp = rrlookup(dp, type, OKneg);
	if(rp)
		if(rp->db){
			/* unauthoritative db entries are hints */
			if(rp->auth) {
				noteinmem();
				if(debug)
					dnslog("[%d] dnresolve1 %s %d %d: auth rr in db",
						getpid(), name, type, class);
				return rp;
			}
		} else
			/* cached entry must still be valid */
			if(rp->ttl > now)
				/* but Tall entries are special */
				if(type != Tall || rp->query == Tall) {
					noteinmem();
					if(debug)
						dnslog("[%d] dnresolve1 %s %d %d: rr not in db",
							getpid(), name, type, class);
					return rp;
				}
	lock(&dnlock);
	rrfreelist(rp);
	unlock(&dnlock);
	rp = nil;		/* accident prevention */
	USED(rp);

	/*
	 * try the cache for a canonical name. if found punt
	 * since we'll find it during the canonical name search
	 * in dnresolve().
	 */
	if(type != Tcname){
		rp = rrlookup(dp, Tcname, NOneg);
		lock(&dnlock);
		rrfreelist(rp);
		unlock(&dnlock);
		if(rp){
			if(debug)
				dnslog("[%d] dnresolve1 %s %d %d: rr from rrlookup for non-cname",
					getpid(), name, type, class);
			return nil;
		}
	}

	/*
	 * if the domain name is within an area of ours,
	 * we should have found its data in memory by now.
	 */
	area = inmyarea(dp->name);
	if (area || strncmp(dp->name, "local#", 6) == 0) {
//		char buf[32];

//		dnslog("%s %s: no data in area %s", dp->name,
//			rrname(type, buf, sizeof buf), area->soarr->owner->name);
		return nil;
	}

	qp = emalloc(sizeof *qp);
	queryinit(qp, dp, type, req);
	rp = issuequery(qp, name, class, depth, recurse);
	querydestroy(qp);
	free(qp);
	if(rp){
		if(debug)
			dnslog("[%d] dnresolve1 %s %d %d: rr from query",
				getpid(), name, type, class);
		return rp;
	}

	/* settle for a non-authoritative answer */
	rp = rrlookup(dp, type, OKneg);
	if(rp){
		if(debug)
			dnslog("[%d] dnresolve1 %s %d %d: rr from rrlookup",
				getpid(), name, type, class);
		return rp;
	}

	/* noone answered.  try the database, we might have a chance. */
	rp = dblookup(name, class, type, 0, 0);
	if (rp) {
		if(debug)
			dnslog("[%d] dnresolve1 %s %d %d: rr from dblookup",
				getpid(), name, type, class);
	}else{
		if(debug)
			dnslog("[%d] dnresolve1 %s %d %d: no rr from dblookup; crapped out",
				getpid(), name, type, class);
	}
	return rp;
}

/*
 *  walk a domain name one element to the right.
 *  return a pointer to that element.
 *  in other words, return a pointer to the parent domain name.
 */
char*
walkup(char *name)
{
	char *cp;

	cp = strchr(name, '.');
	if(cp)
		return cp+1;
	else if(*name)
		return "";
	else
		return 0;
}

/*
 *  Get a udp port for sending requests and reading replies.  Put the port
 *  into "headers" mode.
 */
static char *hmsg = "headers";

int
udpport(char *mtpt)
{
	int fd, ctl;
	char ds[64], adir[64];

	/* get a udp port */
	snprint(ds, sizeof ds, "%s/udp!*!0", (mtpt? mtpt: "/net"));
	ctl = announce(ds, adir);
	if(ctl < 0){
		/* warning("can't get udp port"); */
		return -1;
	}

	/* turn on header style interface */
	if(write(ctl, hmsg, strlen(hmsg)) != strlen(hmsg)){
		close(ctl);
		warning(hmsg);
		return -1;
	}

	/* grab the data file */
	snprint(ds, sizeof ds, "%s/data", adir);
	fd = open(ds, ORDWR);
	close(ctl);
	if(fd < 0)
		warning("can't open udp port %s: %r", ds);
	return fd;
}

void
initdnsmsg(DNSmsg *mp, RR *rp, int flags, ushort reqno)
{
	mp->flags = flags;
	mp->id = reqno;
	mp->qd = rp;
	if(rp != nil)
		mp->qdcount = 1;
}

DNSmsg *
newdnsmsg(RR *rp, int flags, ushort reqno)
{
	DNSmsg *mp;

	mp = emalloc(sizeof *mp);
	initdnsmsg(mp, rp, flags, reqno);
	return mp;
}

static DN edns0owner = {
.name	= "",
.class	= 1478,		/* checksum errors if larger (udp bug?) */
};

static RR edns0rr = {
.magic	= 0,
.db	= 1,		/* use our ttl */
.ttl	= 0,		/* 1<<31 == dnssecok */
.type	= Topt,

.owner	= &edns0owner,
};

/* generate a DNS UDP query packet */
int
mkreq(DN *dp, int type, uchar *buf, int flags, ushort reqno, int edns)
{
	DNSmsg m;
	int len;
	Udphdr *uh = (Udphdr*)buf;
	RR *rp;

	/* stuff port number into output buffer */
	memset(uh, 0, sizeof *uh);
	hnputs(uh->rport, 53);

	/* make request and convert it to output format */
	memset(&m, 0, sizeof m);
	rp = rralloc(type);
	rp->owner = dp;
	initdnsmsg(&m, rp, flags, reqno);
	if(useedns0 && edns){
		m.arcount = 1;
		m.ar = &edns0rr;
	}
	len = convDNS2M(&m, &buf[Udphdrsize], Maxudp);
	rrfreelistptr(&m.qd);
	memset(&m, 0, sizeof m);		/* cause trouble */
	return len;
}

void
freeanswers(DNSmsg *mp)
{
	lock(&dnlock);
	rrfreelistptr(&mp->qd);
	rrfreelistptr(&mp->an);
	rrfreelistptr(&mp->ns);
	rrfreelistptr(&mp->ar);
	unlock(&dnlock);
	mp->qdcount = mp->ancount = mp->nscount = mp->arcount = 0;
}

/* timed read of reply.  sets srcip.  ibuf must be 64K to handle tcp answers. */
static int
readnet(Query *qp, int medium, uchar *ibuf, uvlong endms, uchar **replyp,
	uchar *srcip)
{
	int len, fd;
	long ms;
	vlong startns = nsec();
	uchar *reply;
	uchar lenbuf[2];

	len = -1;			/* pessimism */
	ms = endms - NS2MS(startns);
	if (ms <= 0)
		return -1;		/* taking too long */

	reply = ibuf;
	memset(srcip, 0, IPaddrlen);
	alarm(ms);
	if(medium == Tcp){
		if (!qp->tcpset){
			alarm(0);
			dnslog("readnet: tcp params not set");
			return -1;
		}
		fd = qp->tcpfd;
		if (fd < 0)
			dnslog("readnet: %s: tcp fd unset for dest %I",
				qp->dp->name, qp->tcpip);
		else if (readn(fd, lenbuf, 2) != 2) {
			dnslog("readnet: short read of 2-byte tcp msg size from %I for %s: %r",
				qp->tcpip, qp->dp->name);
			/* probably a time-out */
			notestats(startns, 1, qp->type);
		} else {
			len = lenbuf[0]<<8 | lenbuf[1];
			if (readn(fd, ibuf, len) != len) {
				dnslog("readnet: short read of tcp data from %I",
					qp->tcpip);
				/* probably a time-out */
				notestats(startns, 1, qp->type);
				len = -1;
			}
		}
		memmove(srcip, qp->tcpip, IPaddrlen);
	
	}else
		if (qp->udpfd < 0)
			dnslog("readnet: qp->udpfd closed");
		else {
			len = read(qp->udpfd, ibuf, Udphdrsize+Maxudpin);
			alarm(0);
			notestats(startns, len < 0, qp->type);
			if (len >= IPaddrlen)
				memmove(srcip, ibuf, IPaddrlen);
			if (len >= Udphdrsize) {
				len   -= Udphdrsize;
				reply += Udphdrsize;
			}
		}
	alarm(0);
	*replyp = reply;
	return len;
}

/*
 *  read replies to a request and remember the rrs in the answer(s).
 *  ignore any of the wrong type.
 *  wait at most until endms.
 *  misfeature: should track number of outstanding questions.
 */
static int
readreply(Query *qp, int medium, ushort req, uchar *ibuf, DNSmsg *mp,
	uvlong endms)
{
	int len, rv;
	char *err;
	char tbuf[32], ebuf[32];
	uchar *reply;
	uchar srcip[IPaddrlen];
	RR *rp;

	queryck(qp);
	memset(mp, 0, sizeof *mp);
	memset(srcip, 0, sizeof srcip);
	if (0)
		len = -1;
	for (; timems() < endms &&
	    (len = readnet(qp, medium, ibuf, endms, &reply, srcip)) >= 0;
	    freeanswers(mp)){
		/* convert into internal format  */
		memset(mp, 0, sizeof *mp);
		err = convM2DNS(reply, len, mp, &rv);
		if(rv == Rok)
			/* check for return codes indicating we should quit */
			switch(mp->flags & Rmask){
			case Runimplimented:
			case Rrefused:
			case Rbadvers:
			case Rformat:
				rv = mp->flags & Rmask;
				break;
			}
		if (mp->flags & Ftrunc || rv != Rok) {
			free(err);
			freeanswers(mp);
			/*
			 * notify our caller to retry the query via edns0 or tcp.
			 * note, if we sent multiple queries, and one has an error,
			 * we might miss the second (correct) response
			 */
			if(debug && rv != Rok)
				print("%d udp rv %s"	"\n", qp->req->id, respname(rv, ebuf, sizeof ebuf));
			return -2;
		} else if(err){
			dnslog("readreply: %s: input err, len %d: %s: %I",
				qp->dp->name, len, err, srcip);
			free(err);
			continue;	/* should return an error ... other bugs prevent this */
		}
		if(debug)
			logreply(mediumstr[medium], qp->req->id, srcip, mp);

		/* answering the right question? */
		if(mp->id != req)
			dnslog("%d: id %d instead of %d: %I", qp->req->id,
				mp->id, req, srcip);
		else if(mp->qd == 0)
			dnslog("%d: no question RR: %I", qp->req->id, srcip);
		else if(mp->qd->owner != qp->dp)
			dnslog("%d: owner %s instead of %s: %I", qp->req->id,
				mp->qd->owner->name, qp->dp->name, srcip);
		else if(mp->qd->type != qp->type)
			dnslog("%d: qp->type %d instead of %d: %I",
				qp->req->id, mp->qd->type, qp->type, srcip);
		else {
			/* remember what request this is in answer to */
			for(rp = mp->an; rp; rp = rp->next)
				rp->query = qp->type;
			return 0;
		}
	}
	if (timems() >= endms) {
		;				/* query expired */
	} else if (0) {
		/* this happens routinely when a read times out */
		dnslog("readreply: %s type %s: ns %I read error or eof "
			"(returned %d): %r", qp->dp->name, rrname(qp->type,
			tbuf, sizeof tbuf), srcip, len);
		if (medium == Udp || medium == Udpedns0)
			for (rp = qp->nsrp; rp != nil; rp = rp->next)
				if (rp->type == Tns)
					dnslog("readreply: %s: query sent to "
						"ns %s", qp->dp->name,
						rp->host->name);
	}
	return -1;
}

/*
 *	return non-0 if first list includes second list
 */
int
contains(RR *rp1, RR *rp2)
{
	RR *trp1, *trp2;

	for(trp2 = rp2; trp2; trp2 = trp2->next){
		for(trp1 = rp1; trp1; trp1 = trp1->next)
			if(trp1->type == trp2->type)
			if(trp1->host == trp2->host)
			if(trp1->owner == trp2->owner)
				break;
		if(trp1 == nil)
			return 0;
	}
	return 1;
}


/*
 *  return multicast version if any
 */
int
ipisbm(uchar *ip)
{
	if(isv4(ip)){
		if (ip[IPv4off] >= 0xe0 && ip[IPv4off] < 0xf0 ||
		    ipcmp(ip, IPv4bcast) == 0)
			return 4;
	} else
		if(ip[0] == 0xff)
			return 6;
	return 0;
}

/*
 *  Get next server address(es) into qp->dest[nd] and beyond
 */
static int
serveraddrs(Query *qp, int nd, int depth)
{
	RR *rp, *arp, *trp;
	Dest *cur;

	if(nd >= Maxdest)		/* dest array is full? */
		return Maxdest - 1;

	/*
	 *  look for a server whose address we already know.
	 *  if we find one, mark it so we ignore this on
	 *  subsequent passes.
	 */
	arp = nil;
	for(rp = qp->nsrp; rp; rp = rp->next){
		assert(rp->magic == RRmagic);
		if(rp->marker)
			continue;
		arp = rrlookup(rp->host, Ta, NOneg);
		if(arp == nil)
			arp = rrlookup(rp->host, Taaaa, NOneg);
		if(arp){
			rp->marker = 1;
			break;
		}
		arp = dblookup(rp->host->name, Cin, Ta, 0, 0);
		if(arp == nil)
			arp = dblookup(rp->host->name, Cin, Taaaa, 0, 0);
		if(arp){
			rp->marker = 1;
			break;
		}
	}
	/*
	 *  if the cache and database lookup didn't find any new
	 *  server addresses, try resolving one via the network.
	 *  Mark any we try to resolve so we don't try a second time.
	 */
	if(arp == nil)
		for(rp = qp->nsrp; rp; rp = rp->next){
			if(rp->marker)
				continue;
			rp->marker = 1;

			/*
			 *  avoid loops looking up a server under itself
			 */
			if(subsume(rp->owner->name, rp->host->name))
				continue;

			arp = dnresolve(rp->host->name, Cin, Ta, qp->req, 0,
				depth+1, Recurse, 1, 0);
			if(arp == nil)
				arp = dnresolve(rp->host->name, Cin, Taaaa,
					qp->req, 0, depth+1, Recurse, 1, 0);
			lock(&dnlock);
			rrfreelist(rrremneg(&arp));
			unlock(&dnlock);
			if(arp)
				break;
		}

	/* use any addresses that we found */
	for(trp = arp; trp && nd < Maxdest-1; trp = trp->next){
		cur = &qp->dest[nd];
		parseip(cur->a, trp->ip->name);
		/*
		 * straddling servers can reject all nameservers if they are all
		 * inside, so be sure to list at least one outside ns at
		 * the end of the ns list in /lib/ndb for `dom='.
		 */
		if (ipisbm(cur->a) ||
		    cfg.straddle && !insideaddr(qp->dp->name) && insidens(cur->a))
			continue;
		cur->nx = 0;
		cur->s = trp->owner;
		cur->code = Rtimeout;
		nd++;
	}
	lock(&dnlock);
	rrfreelist(arp);
	unlock(&dnlock);

	desttrace("serveraddrs nd = %d\n", nd);
	return nd;
}

/*
 *  cache negative responses
 */
static void
cacheneg(DN *dp, int type, int rcode, RR *soarr)
{
	RR *rp;
	DN *soaowner;
	ulong ttl;

	stats.negcached++;

	/* no cache time specified, don't make anything up */
	if(soarr != nil){
		lock(&dnlock);
		if(soarr->next != nil)
			rrfreelistptr(&soarr->next);
		unlock(&dnlock);
		soaowner = soarr->owner;
	} else
		soaowner = nil;

	/* the attach can cause soarr to be freed so mine it now */
	if(soarr != nil && soarr->soa != nil)
		ttl = soarr->soa->minttl+now;
	else
		ttl = 5*Min;

	/* add soa and negative RR to the database */
	rrattach(soarr, Authoritative);

	rp = rralloc(type);
	rp->owner = dp;
	rp->negative = 1;
	rp->negsoaowner = soaowner;
	rp->negrcode = rcode;
	rp->ttl = ttl;
	rrattach(rp, Authoritative);
}

static int
setdestoutns(Dest *p, int n)
{
	uchar outns[IPaddrlen];

	destck(p);
	destinit(p);
	if(outsidens(n, outns) == -1){
		if (n == 0)
			dnslog("[%d] no outside-ns in ndb", getpid());
		return -1;
	}
	memmove(p->a, outns, sizeof p->a);
	p->s = dnlookup("outside-ns-ips", Cin, 1);
	return 0;
}

/*
 * issue query via UDP or TCP as appropriate.
 * for TCP, returns with qp->tcpip set from udppkt header.
 */
static int
mydnsquery(Query *qp, int medium, uchar *udppkt, int len)
{
	int rv = -1, nfd;
	char *domain;
	char conndir[40], net[40];
	uchar belen[2];
	NetConnInfo *nci;

	queryck(qp);
	domain = smprint("%I", udppkt);
	if (myaddr(domain)) {
		dnslog("mydnsquery: trying to send to myself (%s); %s %s",
			domain, qp->dp->name, rrtname[qp->type]);
		free(domain);
		return rv;
	}

	switch (medium) {
	case Udp:
	case Udpedns0:
		free(domain);
		nfd = dup(qp->udpfd, -1);
		if (nfd < 0) {
			warning("mydnsquery: qp->udpfd %d: %r", qp->udpfd);
			close(qp->udpfd);	/* ensure it's closed */
			qp->udpfd = -1;		/* poison it */
			return rv;
		}
		close(nfd);

		if (qp->udpfd < 0)
			dnslog("mydnsquery: qp->udpfd %d closed", qp->udpfd);
		else {
			if (write(qp->udpfd, udppkt, len+Udphdrsize) !=
			    len+Udphdrsize)
				warning("sending udp msg: %r");
			else {
				stats.qsent++;
				rv = 0;
			}
		}
		break;
	case Tcp:
		/* send via TCP & keep fd around for reply */
		snprint(net, sizeof net, "%s/tcp",
			(mntpt[0] != '\0'? mntpt: "/net"));
		alarm(10*1000);
		qp->tcpfd = rv = dial(netmkaddr(domain, net, "dns"), nil,
			conndir, &qp->tcpctlfd);
		alarm(0);
		if (qp->tcpfd < 0) {
			dnslog("can't dial %s!%s!dns: %r", net, domain);
			free(domain);
			break;
		}
		free(domain);
		nci = getnetconninfo(conndir, qp->tcpfd);
		if (nci) {
			parseip(qp->tcpip, nci->rsys);
			freenetconninfo(nci);
		} else
			dnslog("mydnsquery: getnetconninfo failed");
		qp->tcpset = 1;

		belen[0] = len >> 8;
		belen[1] = len;
		if (write(qp->tcpfd, belen, 2) != 2 ||
		    write(qp->tcpfd, udppkt + Udphdrsize, len) != len)
			warning("sending tcp msg: %r");
		break;
	default:
		sysfatal("mydnsquery: bad medium");
	}
	return rv;
}

static void
destsum(Query *qp, int j)
{
	int k;
	char *star;
	Dest *d;
	DN *s;

	desttrace("moredest = %d j = %d\n", qp->ndest, j);
	for(k = 0; k < qp->ndest; k++){
		d = qp->dest + k;
		star = k==j? "*": "";
		s = d->s;
		if(d == nil)
			desttrace("dest[%d] %s NIL\n", k, star);
		else{
			if(s == nil)
				desttrace("dest[%d] %s NIL (s is nil)\n", k, star);
			else
				desttrace("dest[%d] %s %s/%I\n", k, star, d->s->name, d->a);
		}
	}
}

void
cachenegq(Query *qp)
{
	RR *soarr;

	soarr = rralloc(Tsoa);
	soarr->owner = dnlookup(qp->dp->name, qp->dp->class, 1);
	soarr->soa->retry = 
		soarr->soa->expire = 
		soarr->soa->minttl =
		soarr->soa->refresh = 120;
	cacheneg(qp->dp, qp->type, Rname, soarr);
}

/*
 * send query to all UDP destinations or one TCP destination,
 * taken from obuf (udp packet) header
 */
static int
xmitquery(Query *qp, int medium, int depth, uchar *obuf, int inns, int len)
{
	int j, n;
	char buf[32];
	Dest *p;

	queryck(qp);
	if(timems() >= qp->req->aborttime)
		return -1;

	/*
	 * get a nameserver address if we need one.
	 * serveraddrs populates qp->dest.
	 */
	p = qp->dest;
	destck(p);
	if (qp->ndest < 0 || qp->ndest > Maxdest) {
		dnslog("qp->ndest %d out of range", qp->ndest);
		abort();
	}
	/*
	 * we're to transmit to more destinations than we currently have,
	 * so get another.
	 */
	if (qp->ndest > qp->curdest - p) {
		j = serveraddrs(qp, qp->curdest - p, depth);
		if(j == -1){
			desttrace("serveraddrs returns -1; depth %d %s\n", depth, qp->dp->name);
			/* prevent bogus cname queries; there's nobody to send them to */
			cachenegq(qp);
			return -1;
		}
		if (j < 0 || j >= Maxdest) {
			dnslog("serveraddrs() result %d out of range", j);
			abort();
		}
		qp->curdest = &qp->dest[j];
		destsum(qp, j);
	}
	destck(qp->curdest);

	/* no servers, punt */
	if (qp->ndest == 0)
		if (cfg.straddle && cfg.inside) {
			/* get ips of "outside-ns-ips" */
			qp->curdest = qp->dest;
			for(n = 0; n < Maxdest; n++, qp->curdest++)
				if (setdestoutns(qp->curdest, n) < 0)
					break;
			if(n == 0)
				dnslog("xmitquery: %s: no outside-ns nameservers",
					qp->dp->name);
		} else
			/* it's probably just a bogus domain, don't log it */
			return -1;

	/* send to first 'qp->ndest' destinations */
	j = 0;
	if (medium == Tcp) {
		queryck(qp);
		assert(qp->dp);
		procsetname("tcp %sside query for %s %s", (inns? "in": "out"),
			qp->dp->name, rrname(qp->type, buf, sizeof buf));
		if(mydnsquery(qp, medium, obuf, len) != -1)
			j++;		/* sets qp->tcpip from obuf */
		if(debug)
			logsend("tcp", qp->req->id, depth, qp->tcpip, "", qp->dp->name,
				qp->type);
	} else
		for(; p < &qp->dest[qp->ndest] && p < qp->curdest; p++){
			/* skip destinations we've finished with */
			if(p->nx >= Maxtrans)
				continue;

			j++;

			/* exponential backoff of requests */
			if((1<<p->nx) > qp->ndest)
				continue;

			if(memcmp(p->a, IPnoaddr, sizeof IPnoaddr) == 0)
				continue;		/* mistake */

			procsetname("%s %sside query to %I/%s %s %s", mediumstr[medium],
				(inns? "in": "out"), p->a, p->s->name,
				qp->dp->name, rrname(qp->type, buf, sizeof buf));
			if(debug)
				logsend(mediumstr[medium], qp->req->id, depth, p->a, p->s->name,
					qp->dp->name, qp->type);

			/* fill in UDP destination addr & send it */
			memmove(obuf, p->a, sizeof p->a);
			mydnsquery(qp, medium, obuf, len);
			p->nx++;
		}
	if(j == 0) {
		return -1;
	}
	return 0;
}

static int lckindex[Maxlcks] = {
	0,			/* all others map here */
	Ta,
	Tns,
	Tcname,
	Tsoa,
	Tptr,
	Tmx,
	Ttxt,
	Taaaa,
};

static int
qtype2lck(int qtype)		/* map query type to querylck index */
{
	int i;

	for (i = 1; i < nelem(lckindex); i++)
		if (lckindex[i] == qtype)
			return i;
	return 0;
}

/* is mp a cachable negative response (with Rname set)? */
static int
isnegrname(DNSmsg *mp)
{
	/* TODO: could add || cfg.justforw to RHS of && */
	return mp->an == nil && (mp->flags & Rmask) == Rname;
}

void
debugwalkup(Query *q, DNSmsg *m)
{
	char *cp;
	DN *d;
	RR *ns;

	ns = nil;
	d = nil;
	for(cp = m->qd->owner->name; cp; cp = walkup(cp)){
		d = dnlookup(cp, m->qd->owner->class, 0);
		if(d == nil)
			continue;

		ns = rrlookup(d, Tns, OKneg);
		if(ns){
			/* don't pass on anything we know is wrong */
			if(ns->negative){
				lock(&dnlock);
				rrfreelist(ns);
				unlock(&dnlock);
			}
			break;
		}
		d = nil;

//		if (strncmp(nsdp->name, "local#", 6) == 0)
//			dnslog("returning %s as nameserver", nsdp->name);
		ns = dblookup(cp, m->qd->owner->class, Tns, 0, 0);
		if(ns)
			break;
	}
	if(ns){
		dnslog("domain %s %s", d!=nil? d->name: nil, ns->negative? "!": "");
		dnslog("%s walkup from [%s] to [%s] %R", q->dp->name, m->qd->owner->name, cp, ns);
		lock(&dnlock);
		rrfreelist(ns);
		unlock(&dnlock);
	}
}

/* returns Answerr (-1) on errors, else number of answers, which can be zero. */
static int
procansw(Query *qp, DNSmsg *mp, uchar *srcip, int depth, Dest *p)
{
	int rv;
//	int lcktype;
	char buf[32];
	DN *ndp;
	Query *nqp;
	RR *tp, *soarr;

	if (mp->an == nil)
		stats.negans++;

	/* ignore any error replies */
	if((mp->flags & Rmask) == Rserver){
		stats.negserver++;
		freeanswers(mp);
		if(p != qp->curdest)
			p->code = Rserver;
		return Answerr;
	}

	/* ignore any bad delegations */
//	if(qp->type != Tcname)		/* ignore out-of-bailiwick cnames */
	if(mp->ns && baddelegation(mp->ns, qp->nsrp, srcip)){
		stats.negbaddeleg++;
		if(mp->an == nil){
			stats.negbdnoans++;
			freeanswers(mp);
			if(p != qp->curdest)
				p->code = Rserver;
			dnslog(" and no answers");
			return Answerr;
		}
		if(mp->an && mp->an->type == Tcname)
			dnslog("out-of-balliwick cname %R", mp->an);
		else{
			dnslog("qp nameserver is %R", qp->nsrp);
			dnslog("mp nameserver is %R", mp->ns);
			dnslog("query type is %s", rrtname[qp->type]);
			dnslog("mp is %R", mp->an);
			for(RR *x = qp->nsrp; x != nil; x = x->next)
				dnslog("  qp  %R", x);
			debugwalkup(qp, mp);
			dnslog(" but has answers; ignoring ns");
		}
		lock(&dnlock);
		rrfreelistptr(&mp->ns);
		unlock(&dnlock);
		mp->nscount = 0;
	}

	/* remove any soa's from the authority section */
	lock(&dnlock);
	soarr = rrremtype(&mp->ns, Tsoa);

	/* incorporate answers */
	unique(mp->an);
	unique(mp->ns);
	unique(mp->ar);
	unlock(&dnlock);

	if(mp->an)
		rrattach(mp->an, (mp->flags & Fauth) != 0);
	if(mp->ar)
		rrattach(mp->ar, Notauthoritative);
	if(mp->ns && !cfg.justforw){
		ndp = mp->ns->owner;
		rrattach(mp->ns, Notauthoritative);
	} else {
		ndp = nil;
		lock(&dnlock);
		rrfreelistptr(&mp->ns);
		unlock(&dnlock);
		mp->nscount = 0;
	}

	/* free the question */
	if(mp->qd) {
		lock(&dnlock);
		rrfreelistptr(&mp->qd);
		unlock(&dnlock);
		mp->qdcount = 0;
	}

	/*
	 *  Any reply from an authoritative server,
	 *  or a positive reply terminates the search.
	 *  A negative response now also terminates the search.
	 */
	if(mp->an != nil || (mp->flags & Fauth)){
		if(isnegrname(mp))
			qp->dp->respcode = Rname;
		else
			qp->dp->respcode = Rok;

		/*
		 *  cache any negative responses, free soarr.
		 *  negative responses need not be authoritative:
		 *  they can legitimately come from a cache.
		 */
		if( /* (mp->flags & Fauth) && */ mp->an == nil)
			cacheneg(qp->dp, qp->type, (mp->flags & Rmask), soarr);
		else {
			lock(&dnlock);
			rrfreelist(soarr);
			unlock(&dnlock);
		}
		return 1;
	} else if (isnegrname(mp)) {
		qp->dp->respcode = Rname;
		/*
		 *  cache negative response.
		 *  negative responses need not be authoritative:
		 *  they can legitimately come from a cache.
		 */
		cacheneg(qp->dp, qp->type, (mp->flags & Rmask), soarr);
		return 1;
	}
	stats.negnorname++;
	lock(&dnlock);
	rrfreelist(soarr);
	unlock(&dnlock);

	/*
	 *  if we've been given better name servers, recurse.
	 *  if we're a pure resolver, don't recurse, we have
	 *  to forward to a fixed set of named servers.
	 */
	if(!mp->ns || cfg.resolver && cfg.justforw)
		return Answnone;
	tp = rrlookup(ndp, Tns, NOneg);
	if(contains(qp->nsrp, tp)){
		lock(&dnlock);
		rrfreelist(tp);
		unlock(&dnlock);
		return Answnone;
	}
	procsetname("recursive query for %s %s", qp->dp->name,
		rrname(qp->type, buf, sizeof buf));
	/*
	 *  we're called from udpquery, called from
	 *  netquery, which current holds qp->dp->querylck,
	 *  so release it now and acquire it upon return.
	 */
//	lcktype = qtype2lck(qp->type);		/* someday try this again */
//	qunlock(&qp->dp->querylck[lcktype]);

	nqp = emalloc(sizeof *nqp);
	queryinit(nqp, qp->dp, qp->type, qp->req);
	nqp->nsrp = tp;
	rv = netquery(nqp, depth+1);
	if(rv == Answerr){
		/*
		 * propogate the error to avoid infinite (or very long recursions)
		 * when we have a negative result
		 */
	//	cacheneg(nqp->dp, nqp->type, (mp->flags & Rmask), soarr);
		rv = Answrequeryerr;
	}

//	qlock(&qp->dp->querylck[lcktype]);
	rrfreelist(nqp->nsrp);
	querydestroy(nqp);
	free(nqp);
	return rv;
}

/*
 * send a query via tcp to a single address (from ibuf's udp header)
 * and read the answer(s) into mp->an.
 */
static int
tcpquery(Query *qp, DNSmsg *mp, int depth, uchar *ibuf, uchar *obuf, int len,
	ulong waitms, int inns, ushort req)
{
	int rv = 0;
	uvlong endms;

	endms = timems() + waitms;
	if(endms > qp->req->aborttime)
		endms = qp->req->aborttime;

	if (0)
		dnslog("%s: udp reply truncated; retrying query via tcp to %I",
			qp->dp->name, qp->tcpip);

	qlock(&qp->tcplock);
	memmove(obuf, ibuf, IPaddrlen);		/* send back to respondent */
	/* sets qp->tcpip from obuf's udp header */
	if (xmitquery(qp, Tcp, depth, obuf, inns, len) < 0 ||
	    readreply(qp, Tcp, req, ibuf, mp, endms) < 0)
		rv = -1;
	if (qp->tcpfd > 0) {
		hangup(qp->tcpctlfd);
		close(qp->tcpctlfd);
		close(qp->tcpfd);
	}
	qp->tcpfd = qp->tcpctlfd = -1;
	qp->tcpset = 0;
	qunlock(&qp->tcplock);
	return rv;
}

/*
 *  query name servers.  fill in obuf with on-the-wire representation of a
 *  DNSmsg derived from qp.  if the name server returns a pointer to another
 *  name server, recurse.
 */
static int
queryns(Query *qp, int depth, uchar *ibuf, uchar *obuf, ulong waitms, int inns)
{
	int ndest, len, replywaits, flags, nx0, rv, xmitabort;
	ushort req;
	uvlong endms;
	char buf[12], *s;
	uchar srcip[IPaddrlen];
	Dest *p, *np, *dest;

	/* pack request into a udp message */
	req = rand();
	flags = Oquery;
	if(cfg.resolver || cfg.justforw)
		flags |= Frecurse;			/* many servers return Srvfail if RD is set */
	len = mkreq(qp->dp, qp->type, obuf, flags, req, 0);

	/* no server addresses yet */
	queryck(qp);
	dest = emalloc(Maxdest * sizeof *dest);	/* dest can't be on stack */
	for (p = dest; p < dest + Maxdest; p++)
		destinit(p);
	/* this dest array is local to this call of queryns() */
	free(qp->dest);
	qp->curdest = qp->dest = dest;

	/*
	 *  transmit udp requests and wait for answers.
	 *  at most Maxtrans attempts to each address.
	 *  each cycle send one more message than the previous.
	 *  retry a query via tcp if its response is truncated.
	 */
	xmitabort = 0;
	for(ndest = 1; ndest < Maxdest; ndest++){
		endms = timems() + waitms;
		if(endms > qp->req->aborttime){
			s = rrname(qp->type, buf, sizeof buf);
			dnslog("%d query timeout: %s %s", qp->req->id, qp->dp->name, s);
			break; //endms = qp->req->aborttime;
		}

		qp->ndest = ndest;
		qp->tcpset = 0;
		nx0 = qp->dest->nx;
		if (xmitquery(qp, Udp, depth, obuf, inns, len) < 0){
			/* no destinations; abort */
			desttrace("xmitquery no destinations; xmitabort\n");
			xmitabort = 1;
			goto abort;
			break;
		}

		for(replywaits = 0; replywaits < ndest; replywaits++){
			DNSmsg m;

			procsetname("reading %sside reply from %I: %s %s from %s",
				(inns? "in": "out"), obuf, qp->dp->name,
				rrname(qp->type, buf, sizeof buf), qp->req->from);

			/* read udp answer into m */
			if ((rv = readreply(qp, Udp, req, ibuf, &m, endms)) >= 0)
				memmove(srcip, ibuf, IPaddrlen);
			else if (rv == -1) {
				freeanswers(&m);
				break;		/* timed out on this dest */
			} else {
				char *prevproto;
				uchar eobuf[1024];
				int elen;

				/* whoops, truncated?  try edns0? */
				prevproto = "udp";
				freeanswers(&m);
				if(useedns0 && m.flags & Ftrunc){
					logtrunc(prevproto, "edns0", qp->req->id, depth, nil, nil, qp->dp->name, qp->type);
					prevproto = "edns0";
					elen = mkreq(qp->dp, qp->type, eobuf, flags, req, 1);
					qp->dest->nx = nx0;
					rv = xmitquery(qp, Udpedns0, depth, eobuf, inns, elen);
					if(debug)
						print("%d.%d: EDNS0 sent rv=%d\n", qp->req->id, depth, rv);
				}else
					rv = -3;

				if(rv < 0)
					rv = -3;
				if(rv == 0 && (rv = readreply(qp, Udpedns0, req, ibuf, &m, endms)) >= 0){
					memmove(srcip, ibuf, IPaddrlen);
					if(0 && debug)
						print("%d.%d: EDNS0 reply\n", qp->req->id, depth);
				}
				else if(rv == -1){
					if(0 && debug)
						print("%d.%d: EDNS0 timeout\n", qp->req->id, depth);
					freeanswers(&m);
					break;		/* timed out on this dest */
				}else{
					logtrunc(prevproto, "tcp", qp->req->id, depth, nil, nil, qp->dp->name, qp->type);
					/* whoops, bad edns0? ask again via tcp */
					freeanswers(&m);
					rv = tcpquery(qp, &m, depth, ibuf, obuf, len,
						waitms, inns, req);  /* answer in m */
					if (rv < 0) {
						freeanswers(&m);
						break;		/* failed via tcp too */
					}
					memmove(srcip, qp->tcpip, IPaddrlen);
				}
			}

			/* find responder */
			// dnslog("queryns got reply from %I", srcip);
			for(p = qp->dest; p < qp->curdest; p++)
				if(memcmp(p->a, srcip, sizeof p->a) == 0)
					break;

			/* remove all addrs of responding server from list */
			for(np = qp->dest; np < qp->curdest; np++)
				if(np->s == p->s)
					np->nx = Maxtrans;

			/* free or incorporate RRs in m */
			rv = procansw(qp, &m, srcip, depth, p);
			if (rv > Answnone) {
				free(qp->dest);
				qp->dest = qp->curdest = nil; /* prevent accidents */
				return rv;
			}
			if(rv == Answrequeryerr){
				desttrace("procansw re-error; xmitabort\n");
				p->code = Rserver;
				xmitabort = 1;
				goto abort;
			}
		}
	}

abort:

	/* if all servers returned failure, propagate it */
	qp->dp->respcode = Rserver;
	for(p = dest; p < qp->curdest; p++) {
		destck(p);
if(xmitabort)p->code = Rserver;
		if(!xmitabort)
		if(p->code != Rserver)
			qp->dp->respcode = Rok;
		p->magic = 0;			/* prevent accidents */
	}

//	if (qp->dp->respcode)
//		dnslog("queryns setting Rserver for %s", qp->dp->name);

	free(qp->dest);
	qp->dest = qp->curdest = nil;		/* prevent accidents */
	if(xmitabort)
		return Answerr;
	return Answnone;
}

/*
 *  run a command with a supplied fd as standard input
 */
char *
system(int fd, char *cmd)
{
	int pid, p, i;
	static Waitmsg msg;

	if((pid = fork()) == -1)
		sysfatal("fork failed: %r");
	else if(pid == 0){
		dup(fd, 0);
		close(fd);
		for (i = 3; i < 200; i++)
			close(i);		/* don't leak fds */
		execl("/bin/rc", "rc", "-c", cmd, nil);
		sysfatal("exec rc: %r");
	}
	for(p = waitpid(); p >= 0; p = waitpid())
		if(p == pid)
			return msg.msg;
	return "lost child";
}

/* compute wait, weighted by probability of success, with bounds */
static ulong
weight(ulong ms, unsigned pcntprob)
{
	ulong wait;

	wait = (ms * pcntprob) / 100;
	if (wait < Minwaitms)
		wait = Minwaitms;
	if (wait > Maxwaitms)
		wait = Maxwaitms;
	return wait;
}

/*
 * in principle we could use a single descriptor for a udp port
 * to send all queries and receive all the answers to them,
 * but we'd have to sort out the answers by dns-query id.
 */
static int
udpquery(Query *qp, char *mntpt, int depth, int patient, int inns)
{
	int fd, rv;
	long now;
	ulong pcntprob;
	uvlong wait, reqtm;
	char *msg;
	uchar *obuf, *ibuf;
	static QLock mntlck;
	static ulong lastmount;

	/* use alloced buffers rather than ones from the stack */
	ibuf = emalloc(64*1024);		/* max. tcp reply size */
	obuf = emalloc(Maxudp+Udphdrsize);

	fd = udpport(mntpt);
	while (fd < 0 && cfg.straddle && strcmp(mntpt, "/net.alt") == 0) {
		/* HACK: remount /net.alt */
		now = time(nil);
		if (now < lastmount + Remntretry)
			sleep(S2MS(lastmount + Remntretry - now));
		qlock(&mntlck);
		fd = udpport(mntpt);	/* try again under lock */
		if (fd < 0) {
			dnslog("[%d] remounting /net.alt", getpid());
			unmount(nil, "/net.alt");

			msg = system(open("/dev/null", ORDWR), "outside");

			lastmount = time(nil);
			if (msg && *msg) {
				dnslog("[%d] can't remount /net.alt: %s",
					getpid(), msg);
				sleep(10*1000);	/* don't spin remounting */
			} else
				fd = udpport(mntpt);
		}
		qunlock(&mntlck);
	}
	if (fd < 0) {
		dnslog("can't get udpport for %s query of name %s: %r",
			mntpt, qp->dp->name);
		sysfatal("out of udp conversations");	/* we're buggered */
	}

	/*
	 * Our QIP servers are busted and respond to AAAA and CNAME queries
	 * with (sometimes malformed [too short] packets and) no answers and
	 * just NS RRs but not Rname errors.  so make time-to-wait
	 * proportional to estimated probability of an RR of that type existing.
	 */
	if (qp->type >= nelem(likely))
		pcntprob = 35;			/* unpopular query type */
	else
		pcntprob = likely[qp->type];
	reqtm = (patient? 2 * Maxreqtm: Maxreqtm);
	wait = weight(reqtm / 3, pcntprob);	/* time for one udp query */
	qp->req->aborttime = timems() + 3*wait; /* for all udp queries */

	qp->udpfd = fd;
	rv = queryns(qp, depth, ibuf, obuf, wait, inns);
	close(fd);
	qp->udpfd = -1;

	free(obuf);
	free(ibuf);
	return rv;
}

/*
 * look up (qp->dp->name, qp->type) rr in dns,
 * using nameservers in qp->nsrp.
 */
static int
netquery(Query *qp, int depth)
{
	int lock, rv, triedin, inname;
//	char buf[32];
	RR *rp;
	DN *dp;
	Querylck *qlp;
	static int whined;

	rv = Answnone;			/* pessimism */
	if(depth > 12)			/* in a recursive loop? */
		return Answnone;

	slave(qp->req);
	/*
	 * slave might have forked.  if so, the parent process longjmped to
	 * req->mret; we're usually the child slave, but if there are too
	 * many children already, we're still the same process.
	 */

	/*
	 * don't lock before call to slave so only children can block.
	 * just lock at top-level invocation.
	 */
	lock = depth <= 1 && qp->req->isslave;
	dp = qp->dp;		/* ensure that it doesn't change underfoot */
	qlp = nil;
	if(lock) {
//		procsetname("query lock wait: %s %s from %s", dp->name,
//			rrname(qp->type, buf, sizeof buf), qp->req->from);
		/*
		 * don't make concurrent queries for this name.
		 * dozens of processes blocking here probably indicates
		 * an error in our dns data that causes us to not
		 * recognise a zone (area) as one of our own, thus
		 * causing us to query other nameservers.
		 */
		qlp = &dp->querylck[qtype2lck(qp->type)];
		qlock(qlp);
		if (qlp->Ref.ref > Maxoutstanding) {
			qunlock(qlp);
			if (!whined) {
				whined = 1;
				dnslog("too many outstanding queries for %s;"
					" dropping this one; no further logging"
					" of drops", dp->name);
			}
			return 0;
		}
		++qlp->Ref.ref;
		qunlock(qlp);
	}
	procsetname("netquery: %s", dp->name);

	/* prepare server RR's for incremental lookup */
	for(rp = qp->nsrp; rp; rp = rp->next)
		rp->marker = 0;

	triedin = 0;

	/*
	 * normal resolvers and servers will just use mntpt for all addresses,
	 * even on the outside.  straddling servers will use mntpt (/net)
	 * for inside addresses and /net.alt for outside addresses,
	 * thus bypassing other inside nameservers.
	 */
	inname = insideaddr(dp->name);
	if (!cfg.straddle || inname) {
		rv = udpquery(qp, mntpt, depth, Hurry, (cfg.inside? Inns: Outns));
		triedin = 1;
	}

	/*
	 * if we're still looking, are inside, and have an outside domain,
	 * try it on our outside interface, if any.
	 */
	if (rv == Answnone && cfg.inside && !inname) {
		if (triedin)
			dnslog(
	   "[%d] netquery: internal nameservers failed for %s; trying external",
				getpid(), dp->name);

		/* prepare server RR's for incremental lookup */
		for(rp = qp->nsrp; rp; rp = rp->next)
			rp->marker = 0;

		rv = udpquery(qp, "/net.alt", depth, Patient, Outns);
	}
//	if (rv == Answnone)		/* could ask /net.alt/dns directly */
//		askoutdns(dp, qp->type);

	if(lock && qlp) {
		qlock(qlp);
		assert(qlp->Ref.ref > 0);
		qunlock(qlp);
		decref(qlp);
	}
	return rv;
}

int
seerootns(void)
{
	int rv;
	char root[] = "";
	Request req;
	RR *rr;
	Query *qp;

	memset(&req, 0, sizeof req);
	req.isslave = 1;
	req.aborttime = timems() + Maxreqtm;
	req.from = "internal";

	qp = emalloc(sizeof *qp);
	queryinit(qp, dnlookup(root, Cin, 1), Tns, &req);
	qp->nsrp = dblookup(root, Cin, Tns, 0, 0);
	for (rr = qp->nsrp; rr != nil; rr = rr->next)	/* DEBUG */
		dnslog("seerootns query nsrp: %R", rr);

	rv = netquery(qp, 0);		/* lookup ". ns" using qp->nsrp */

	rrfreelist(qp->nsrp);
	querydestroy(qp);
	free(qp);
	return rv;
}

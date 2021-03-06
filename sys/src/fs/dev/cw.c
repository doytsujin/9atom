#include "all.h"
#include "mem.h"		// for Ticks.

#define dprint(...)
#define dpprint(...)	//print(__VA_ARGS__)

#define	FIRST		SUPER_ADDR

#define	ADDFREE		(100)
#define	CACHE_ADDR	SUPER_ADDR
#define	MAXAGE		10000

#define	CDEV(d)		(d->cw.c)
#define	WDEV(d)		(d->cw.w)
#define	RDEV(d)		(d->cw.ro)

/* cache state */
enum
{
	/* states -- beware these are recorded on the cache */
				/*    cache    worm	*/
	Cnone = 0,		/*	0	?	*/
	Cdirty,			/*	1	0	*/
	Cdump,			/*	1	0->1	*/
	Cread,			/*	1	1	*/
	Cwrite,			/*	2	1	*/
	Cdump1,			/* inactive form of dump */
	Cerror,

	/* opcodes -- these are not recorded */
	Onone,
	Oread,
	Owrite,
	Ogrow,
	Odump,
	Orele,
	Ofree,
};

typedef	struct	Cw	Cw;
struct	Cw
{
	Device*	dev;
	Device*	cdev;
	Device*	wdev;
	Device*	rodev;
	Cw*	link;

	Filter	ncwio;
	int	dbucket;	/* last bucket dumped */
	Off	daddr;		/* last block dumped */
	Off	ncopy;
	int	nodump;
/*
 * following are cached variables for dumps
 */
	Off	fsize;
	Off	ndump;
	int	depth;
	int	all;		/* local flag to recur on modified directories */
	int	allflag;	/* global flag to recur on modified directories */
	Off	falsehits;	/* times recur found modified blocks */
	struct
	{
		char	name[500];
		char	namepad[NAMELEN+10];
	};
};

static
char*	cwnames[] =
{
	[Cnone]		"none",
	[Cdirty]	"dirty",
	[Cdump]		"dump",
	[Cread]		"read",
	[Cwrite]	"write",
	[Cdump1]	"dump1",
	[Cerror]	"error",

	[Onone]		"none",
	[Oread]		"read",
	[Owrite]	"write",
	[Ogrow]		"grow",
	[Odump]		"dump",
	[Orele]		"rele",
};

Centry*	getcentry(Bucket*, Off);
int	cwio(Device*, Off, void*, int);
void	cmd_cwcmd(int, char*[]);

/*
 * console command
 * initiate a dump
 */
extern Timet nextdump(Timet);

void
cmd_dump(int argc, char *argv[])
{
	Filsys *fs;

	fs = cons.curfs;
	if(argc > 1)
		fs = fsstr(argv[1]);
	if(fs == 0) {
		print("%s: unknown file system\n", argv[1]);
		return;
	}
	cfsdump(fs);
	nextdump(time());
}

void
cmd_dumpctl(int argc, char *argv[])
{
	char *s;

	if(argc == 2){
		if(strcmp(argv[1], "on") == 0)
			conf.nodump = 0;
		else
			conf.nodump = 1;
	}
	s = "on\0off"+conf.nodump*3;
	print("automatic dumps are %s\n", s);
}

/*
 * console command
 * worm stats
 */
static
void
cmd_statw(int, char*[])
{
	Filsys *fs;
	Iobuf *p;
	Superb *sb;
	Cache *h;
	Bucket *b;
	Centry *c, *ce;
	Off m, nw, bw, state[Onone];
	Off sbfsize, sbcwraddr, sbroraddr, sblast, sbnext;
	Off hmsize, hmaddr, dsize, dsizepct;
	Device *dev;
	Cw *cw;
	int s;

	fs = cons.curfs;
	dev = fs->dev;
	if(dev->type != Devcw) {
		print("curfs not type cw\n");
		return;
	}

	cw = dev->private;
	if(cw == 0) {
		print("curfs not inited\n");
		return;
	}

	print("cwstats %s\n", fs->name);

	sbfsize = 0;
	sbcwraddr = 0;
	sbroraddr = 0;
	sblast = 0;
	sbnext = 0;

	print("	filesys %s\n", fs->name);
	print("	nio   =%W\n", &cw->ncwio);
	p = getbuf(dev, cwsaddr(dev), Bread);
	if(!p || checktag(p, Tsuper, QPSUPER)) {
		print("cwstats: checktag super\n");
		if(p) {
			putbuf(p);
			p = 0;
		}
	}
	if(p) {
		sb = (Superb*)p->iobuf;
		sbfsize = sb->fsize;
		sbcwraddr = sb->cwraddr;
		sbroraddr = sb->roraddr;
		sblast = sb->last;
		sbnext = sb->next;
		putbuf(p);
	}

	p = getbuf(cw->cdev, CACHE_ADDR, Bread|Bres);
	if(!p || checktag(p, Tcache, QPSUPER)) {
		print("cwstats: checktag c bucket\n");
		if(p)
			putbuf(p);
		return;
	}
	h = (Cache*)p->iobuf;
	hmaddr = h->maddr;
	hmsize = h->msize;

	print("		maddr  = %8lld\n", (Wideoff)hmaddr);
	print("		msize  = %8lld\n", (Wideoff)hmsize);
	print("		caddr  = %8lld\n", (Wideoff)h->caddr);
	print("		csize  = %8lld\n", (Wideoff)h->csize);
	print("		sbaddr = %8lld\n", (Wideoff)h->sbaddr);
	print("		craddr = %8lld %8lld\n", (Wideoff)h->cwraddr, (Wideoff)sbcwraddr);
	print("		roaddr = %8lld %8lld\n", (Wideoff)h->roraddr, (Wideoff)sbroraddr);
	/* print stats in terms of (first-)disc sides */
	dsize = wormsizeside(dev, 0);
	if (dsize < 1) {
		dprint("wormsizeside sz %lld for %Z side 0\n", (Wideoff)dsize, dev);
		dsize = h->wsize;	/* it's probably a fake worm */
		if (dsize < 1)
			dsize = 1000;	/* don't divide by zero */
	}
	dsizepct = dsize/100;
	print("		fsize  = %8lld %8lld %2lld+%2lld%%\n", (Wideoff)h->fsize,
				(Wideoff)sbfsize, (Wideoff)h->fsize/dsize,
				(Wideoff)(h->fsize%dsize)/dsizepct);
	print("		slast  =          %8lld\n", (Wideoff)sblast);
	print("		snext  =          %8lld\n", (Wideoff)sbnext);
	print("		wmax   = %8lld          %2lld+%2lld%%\n", (Wideoff)h->wmax,
				(Wideoff)h->wmax/dsize,
				(Wideoff)(h->wmax%dsize)/dsizepct);
	print("		wsize  = %8lld          %2lld+%2lld%%\n", (Wideoff)h->wsize,
				(Wideoff)h->wsize/dsize,
				(Wideoff)(h->wsize%dsize)/dsizepct);
	putbuf(p);

	bw = 0;	/* max filled bucket */
	memset(state, 0, sizeof(state));
	for(m=0; m<hmsize; m++) {
		p = getbuf(cw->cdev, hmaddr + m/BKPERBLK, Bread);
		if(!p || checktag(p, Tbuck, hmaddr + m/BKPERBLK)) {
			print("cwstats: checktag c bucket\n");
			if(p)
				putbuf(p);
			return;
		}
		b = (Bucket*)p->iobuf + m%BKPERBLK;
		ce = b->entry + CEPERBK;
		nw = 0;
		for(c=b->entry; c<ce; c++) {
			s = c->state;
			state[s]++;
			if(s != Cnone && s != Cread)
				nw++;
		}
		putbuf(p);
		if(nw > bw)
			bw = nw;
	}

	for(s=Cnone; s<=Cerror; s++)
		print("		%8lld %s\n", (Wideoff)state[s], cwnames[s]);
	print("		cache %2lld%% full\n", ((Wideoff)bw*100)/CEPERBK);
}

int
dumpblock(Device *dev)
{
	Iobuf *p, *cb, *p1, *p2;
	Cache *h;
	Centry *c, *ce, *bc;
	Bucket *b;
	Off m, a, msize, maddr, wmax, caddr;
	int s1, count;
	Cw *cw;

	cw = dev->private;
	if(cw == 0 || cw->nodump)
		return 0;

	cb = getbuf(cw->cdev, CACHE_ADDR, Bread|Bres);
	h = (Cache*)cb->iobuf;
	msize = h->msize;
	maddr = h->maddr;
	wmax = h->wmax;
	caddr = h->caddr;
	putbuf(cb);

	for(m=msize; m>=0; m--) {
		a = cw->dbucket + 1;
		if(a < 0 || a >= msize)
			a = 0;
		cw->dbucket = a;
		p = getbuf(cw->cdev, maddr + a/BKPERBLK, Bread);
		b = (Bucket*)p->iobuf + a%BKPERBLK;
		ce = b->entry + CEPERBK;
		bc = 0;
		for(c=b->entry; c<ce; c++)
			if(c->state == Cdump) {
				if(bc == 0) {
					bc = c;
					continue;
				}
				if(c->waddr < cw->daddr) {
					if(bc->waddr < cw->daddr &&
					   bc->waddr > c->waddr)
						bc = c;
					continue;
				}
				if(bc->waddr < cw->daddr ||
				   bc->waddr > c->waddr)
					bc = c;
			}
		if(bc) {
			c = bc;
			goto found;
		}
		putbuf(p);
	}
	if(cw->ncopy) {
		print("%lld blocks copied to worm\n", (Wideoff)cw->ncopy);
		cw->ncopy = 0;
	}
	cw->nodump = 1;
	return 0;

found:
/////	a = a*CEPERBK + (c - b->entry) + caddr;
	a = a + (c - b->entry)*msize + caddr;
	p1 = getbuf(devnone, Cwdump1, 0);
	count = 0;

retry:
	count++;
	if(count > 10)
		goto stop;
	dpprint("dumpblock %lld\n", a);
	if(devread(cw->cdev, a, p1->iobuf))
		goto stop;
	m = c->waddr;
	cw->daddr = m;
	s1 = devwrite(cw->wdev, m, p1->iobuf);
	if(s1)
		goto retry;
	/*
	 * reread and compare
	 */
	if(conf.dumpreread) {
		p2 = getbuf(devnone, Cwdump2, 0);
		s1 = devread(cw->wdev, m, p2->iobuf);
		if(s1)
			goto stop1;
		if(memcmp(p1->iobuf, p2->iobuf, RBUFSIZE)) {
			print("reread C%lld W%lld didnt compare\n", (Wideoff)a, (Wideoff)m);
			goto stop1;
		}
		putbuf(p2);
	}

	putbuf(p1);
	if(conf.fastworm)
		c->state = Cnone;
	else
		c->state = Cread;
	p->flags |= Bmod;
	putbuf(p);

	if(m > wmax) {
		cb = getbuf(cw->cdev, CACHE_ADDR, Bread|Bmod|Bres);
		h = (Cache*)cb->iobuf;
		if(m > h->wmax)
			h->wmax = m;
		putbuf(cb);
	}
	cw->ncopy++;
	return 1;

stop1:
	putbuf(p2);
	putbuf(p1);
	c->state = Cdump1;
	p->flags |= Bmod;
	putbuf(p);
	return 1;

stop:
	putbuf(p1);
	putbuf(p);
	print("stopping dump!!\n");
	cw->nodump = 1;
	return 0;
}

void
cwinit1(Device *dev)
{
	Cw *cw;
	static int first;

	cw = dev->private;
	if(cw)
		return;

	if(first++ == 0){
		cmd_install("dump", "-- make dump backup to worm", cmd_dump);
		cmd_install("dumpctl", "[on|off] -- automatic dumps", cmd_dumpctl);
		cmd_install("statw", "-- cache/worm stats", cmd_statw);
		cmd_install("cwcmd", "subcommand -- cache/worm errata", cmd_cwcmd);
		roflag = flag_install("ro", "-- ro reads and writes");
	}
	cw = ialloc(sizeof(Cw), 0);
	dev->private = cw;

	cw->allflag = 0;
	dofilter(&cw->ncwio);

	cw->dev = dev;
	cw->cdev = CDEV(dev);
	cw->wdev = WDEV(dev);
	cw->rodev = RDEV(dev);

	devinit(cw->cdev);
	devinit(cw->wdev);
}

void
cwinit(Device *dev)
{
	Cw *cw;
	Cache *h;
	Iobuf *cb, *p;
	Off l, m;

	cwinit1(dev);

	cw = dev->private;
	l = devsize(cw->wdev);
	cb = getbuf(cw->cdev, CACHE_ADDR, Bread|Bmod|Bres);
	h = (Cache*)cb->iobuf;
	h->toytime = toytime() + SECOND(30);
	h->time = time();
	m = h->wsize;
	if(l != m) {
		print("wdev changed size %lld to %lld\n", (Wideoff)m, (Wideoff)l);
		h->wsize = l;
		cb->flags |= Bmod;
	}

	for(m=0; m<h->msize; m++) {
		p = getbuf(cw->cdev, h->maddr + m/BKPERBLK, Bread);
		if(!p || checktag(p, Tbuck, h->maddr + m/BKPERBLK))
			panic("cwinit: checktag c bucket");
		putbuf(p);
	}
	putbuf(cb);
}

Off
cwsaddr(Device *dev)
{
	Iobuf *cb;
	Off sa;

	cb = getbuf(CDEV(dev), CACHE_ADDR, Bread|Bres);
	sa = ((Cache*)cb->iobuf)->sbaddr;
	putbuf(cb);
	return sa;
}

Off
cwraddr(Device *dev)
{
	Iobuf *cb;
	Off ra;

	switch(dev->type) {
	default:
		print("unknown dev in cwraddr %Z\n", dev);
		return 1;

	case Devcw:
		cb = getbuf(CDEV(dev), CACHE_ADDR, Bread|Bres);
		ra = ((Cache*)cb->iobuf)->cwraddr;
		break;

	case Devro:
		cb = getbuf(CDEV(dev->ro.parent), CACHE_ADDR, Bread|Bres);
		ra = ((Cache*)cb->iobuf)->roraddr;
		break;
	}
	putbuf(cb);
	return ra;
}

Devsize
cwsize(Device *dev)
{
	Iobuf *cb;
	Devsize fs;

	cb = getbuf(CDEV(dev), CACHE_ADDR, Bread|Bres);
	fs = ((Cache*)cb->iobuf)->fsize;
	putbuf(cb);
	return fs;
}

int
cwread(Device *dev, Off b, void *c)
{
	return cwio(dev, b, c, Oread) == Cerror;
}

int
cwwrite(Device *dev, Off b, void *c)
{
	return cwio(dev, b, c, Owrite) == Cerror;
}

int
roread(Device *dev, Off b, void *c)
{
	Device *d;
	int s;

	/*
	 * maybe better is to try buffer pool first
	 */
	d = dev->ro.parent;
	if(d == 0 || d->type != Devcw ||
	   d->private == 0 || RDEV(d) != dev) {
		print("bad rodev %Z\n", dev);
		return 1;
	}
	s = cwio(d, b, 0, Onone);
	if(s == Cdump || s == Cdump1 || s == Cread) {
		s = cwio(d, b, c, Oread);
		if(s == Cdump || s == Cdump1 || s == Cread) {
			if(cons.flags & roflag)
				print("roread: %Z %lld -> %Z(hit)\n", dev, (Wideoff)b, d);
			return 0;
		}
	}
	if(cons.flags & roflag)
		print("roread: %Z %lld -> %Z(miss)\n", dev, (Wideoff)b, WDEV(d));
	return devread(WDEV(d), b, c);
}

static Timet tick0;
int
rowrite(Device *d, Off b, void*)
{
	/* don't dos yourself */
	if(Ticks-tick0 < 5*HZ)
		return 1;
	tick0 = Ticks;
	print("write to ro device %Z(%lld)\n", d, (Wideoff)b);
	return 1;
}

void
roinit(Device *d)
{
	devinit(d->ro.parent);
}

int
cwio(Device *dev, Off addr, void *buf, int opcode)
{
	Iobuf *p, *p1, *cb;
	Cache *h;
	Bucket *b;
	Centry *c;
	Off bn, a1, a2, max, newmax;
	int state;
	Cw *cw;
	long ticks;

ticks = Ticks;

	cw = dev->private;
	cw->ncwio.count++;

	cb = getbuf(cw->cdev, CACHE_ADDR, Bread|Bres);
	h = (Cache*)cb->iobuf;
	if(toytime() >= h->toytime) {
		cb->flags |= Bmod;
		h->toytime = toytime() + SECOND(30);
		h->time = time();
	}

	if(addr < 0) {
		putbuf(cb);
		print("cache error\n");
		return Cerror;
	}

	if(h->msize == 0)
		panic("cwio: h->msize is zero %lld\n", addr);

	bn = addr % h->msize;
	a1 = h->maddr + bn/BKPERBLK;
/////	a2 = bn*CEPERBK + h->caddr;
	a2 = bn + h->caddr;
	max = h->wmax;

	putbuf(cb);
	newmax = 0;

	p = getbuf(cw->cdev, a1, Bread|Bmod);
	if(!p || checktag(p, Tbuck, a1))
		panic("cwio: checktag c bucket");
	b = (Bucket*)p->iobuf + bn%BKPERBLK;

cons.cwio[0] += Ticks-ticks;
ticks = Ticks;

	c = getcentry(b, addr);
	if(c == 0) {
		putbuf(p);
		print("%Z disk cache bucket %lld is full\n", cw->cdev, (Wideoff)a1);
		return Cerror;
	}
//////	a2 += c - b->entry;
	a2 += (c - b->entry)*h->msize;

cons.cwio[1] += Ticks-ticks;
ticks = Ticks;

	state = c->state;
	switch(opcode)
	{
	default:
		goto bad;

	case Onone:
		break;

	case Oread:
		switch(state) {
		default:
			goto bad;

		case Cread:
			if(!devread(cw->cdev, a2, buf))
				break;
			c->state = Cnone;

		case Cnone:
			if(devread(cw->wdev, addr, buf)) {
				state = Cerror;
				break;
			}
			if(addr > max)
				newmax = addr;
			if(conf.fastworm == 0)
			if(!devwrite(cw->cdev, a2, buf))
				c->state = Cread;
			break;

		case Cdirty:
		case Cdump:
		case Cdump1:
		case Cwrite:
			if(devread(cw->cdev, a2, buf))
				state = Cerror;
			break;
		}
		break;

	case Owrite:
		switch(state) {
		default:
			goto bad;

		case Cdump:
		case Cdump1:
			/*
			 * this is hard part -- a dump block must be
			 * sent to the worm if it is rewritten.
			 * if this causes an error, there is no
			 * place to save the dump1 data. the block
			 * is just reclassified as 'dump1' (botch)
			 */
			p1 = getbuf(devnone, Cwio1, 0);
			if(devread(cw->cdev, a2, p1->iobuf)) {
				putbuf(p1);
				print("cwio: write induced dump error - r cache\n");

			casenone:
				if(devwrite(cw->cdev, a2, buf)) {
					state = Cerror;
					break;
				}
				c->state = Cdump1;
				break;
			}
			if(devwrite(cw->wdev, addr, p1->iobuf)) {
				putbuf(p1);
				print("cwio: write induced dump error - r+w worm\n");
				goto casenone;
			}
			putbuf(p1);
			if(conf.fastworm == 0)
				c->state = Cread;
			else
				c->state = Cnone;
			if(addr > max)
				newmax = addr;
			cw->ncopy++;

		case Cnone:
		case Cread:
			if(devwrite(cw->cdev, a2, buf)) {
				state = Cerror;
				break;
			}
			c->state = Cwrite;
			break;

		case Cdirty:
		case Cwrite:
			if(devwrite(cw->cdev, a2, buf))
				state = Cerror;
			break;
		}
		break;

	case Ogrow:
		if(state != Cnone) {
			print("%Z for block %lld cwgrow with state = %s\n",
				cw->cdev, (Wideoff)addr, cwnames[state]);
			break;
		}
		c->state = Cdirty;
		break;

	case Odump:
		if(state != Cdirty) {	/* BOTCH */
			print("%Z for block %lld cwdump with state = %s\n",
				cw->cdev, (Wideoff)addr, cwnames[state]);
			break;
		}
		dpprint("dump %lld\n", addr);
		c->state = Cdump;
		cw->ndump++;	/* only called from dump command */
		break;

	case Orele:
		if(state != Cwrite) {
			if(state != Cdump1)
				print("%Z for block %lld cwrele with state = %s\n",
					cw->cdev, (Wideoff)addr, cwnames[state]);
			break;
		}
		c->state = Cnone;
		break;

	case Ofree:
		if(state == Cwrite || state == Cread)
			c->state = Cnone;
		break;
	}
	dprint("cwio: %Z %lld s=%s o=%s ns=%s\n",
		dev, (Wideoff)addr, cwnames[state],
		cwnames[opcode], cwnames[c->state]);
	putbuf(p);
	if(newmax) {
		cb = getbuf(cw->cdev, CACHE_ADDR, Bread|Bmod|Bres);
		h = (Cache*)cb->iobuf;
		if(newmax > h->wmax)
			h->wmax = newmax;
		putbuf(cb);
	}
cons.cwio[2] += Ticks-ticks;
	return state;

bad:
	print("%Z block %lld cw state = %s; cw opcode = %s",
		dev, (Wideoff)addr, cwnames[state], cwnames[opcode]);
	return Cerror;
}

extern Filsys* dev2fs(Device *dev);

int
cwgrow(Device *dev, Superb *sb, int /*uid*/)
{
	Iobuf *cb;
	Cache *h;
	Off fs, nfs, ws;
	static int c;

	cb = getbuf(CDEV(dev), CACHE_ADDR, Bread|Bmod|Bres);
	h = (Cache*)cb->iobuf;
	ws = h->wsize;
	fs = h->fsize;
	if(fs >= ws)
		return 0;
	nfs = fs + ADDFREE;
	if(nfs >= ws)
		nfs = ws;
	h->fsize = nfs;
	putbuf(cb);

	sb->fsize = nfs;
	for(nfs--; nfs>=fs; nfs--) {
		switch(cwio(dev, nfs, 0, Ogrow)) {
		case Cerror:
			return 0;
		case Cnone:
			addfree(dev, nfs, sb);
		}
	}
	return 1;
}

int
cwfree(Device *dev, Off addr)
{
	int state;

	if(dev->type == Devcw) {
		state = cwio(dev, addr, 0, Ofree);
		if(state != Cdirty)
			return 1;	/* do not put in freelist */
	}
	return 0;			/* put in freelist */
}

int
bktcheck(Bucket *b)
{
	Centry *c, *c1, *c2, *ce;
	int err;

	err = 0;
	if(b->agegen < CEPERBK || b->agegen > MAXAGE) {
		print("agegen %ld\n", b->agegen);
		err = 1;
	}

	ce = b->entry + CEPERBK;
	c1 = 0;		/* lowest age last pass */
	for(;;) {
		c2 = 0;		/* lowest age this pass */
		for(c = b->entry; c < ce; c++) {
			if(c1 != 0 && c != c1) {
				if(c->age == c1->age) {
					print("same age %d\n", c->age);
					err = 1;
				}
				if(c1->waddr == c->waddr)
				if(c1->state != Cnone)
				if(c->state != Cnone) {
					print("same waddr %lld\n", (Wideoff)c->waddr);
					err = 1;
				}
			}
			if(c1 != 0 && c->age <= c1->age)
				continue;
			if(c2 == 0 || c->age < c2->age)
				c2 = c;
		}
		if(c2 == 0)
			break;
		c1 = c2;
		if(c1->age >= b->agegen) {
			print("age >= generator %d %ld\n", c1->age, b->agegen);
			err = 1;
		}
	}
	return err;
}

void
resequence(Bucket *b)
{
	Centry *c, *ce, *cr;
	int age, i;

	ce = b->entry + CEPERBK;
	for(c = b->entry; c < ce; c++) {
		c->age += CEPERBK;
		if(c->age < CEPERBK)
			c->age = MAXAGE;
	}
	b->agegen += CEPERBK;

	age = 0;
	for(i=0;; i++) {
		cr = 0;
		for(c = b->entry; c < ce; c++) {
			if(c->age < i)
				continue;
			if(cr == 0 || c->age < age) {
				cr = c;
				age = c->age;
			}
		}
		if(cr == 0)
			break;
		cr->age = i;
	}
	b->agegen = i;
	cons.nreseq++;
}

Centry*
getcentry(Bucket *b, Off addr)
{
	Centry *c, *ce, *cr;
	int s, age;

	/*
	 * search for cache hit
	 * find oldest block as byproduct
	 */
	ce = b->entry + CEPERBK;
	age = 0;
	cr = 0;
	for(c = b->entry; c < ce; c++) {
		s = c->state;
		if(s == Cnone) {
			cr = c;
			age = 0;
			continue;
		}
		if(c->waddr == addr)
			goto found;
		if(s == Cread) {
			if(cr == 0 || c->age < age) {
				cr = c;
				age = c->age;
			}
		}
	}

	/*
	 * remap entry
	 */
	c = cr;
	if(c == 0)
		return 0;	/* bucket is full */

	c->state = Cnone;
	c->waddr = addr;

found:
	/*
	 * update the age to get filo cache.
	 * small number in age means old
	 */
	if(!cons.noage || c->state == Cnone) {
		age = b->agegen;
		c->age = age;
		age++;
		b->agegen = age;
		if(age < 0 || age >= MAXAGE)
			resequence(b);
	}
	return c;
}

/*
 * ream the cache
 * calculate new buckets
 */
Iobuf*
cacheinit(Device *dev)
{
	Iobuf *cb, *p;
	Cache *h;
	Device *cdev;
	Off m;

	print("cache init %Z\n", dev);
	cdev = CDEV(dev);
	devinit(cdev);

	cb = getbuf(cdev, CACHE_ADDR, Bmod|Bres);
	memset(cb->iobuf, 0, RBUFSIZE);
	settag(cb, Tcache, QPSUPER);
	h = (Cache*)cb->iobuf;

	/*
	 * calculate csize such that
	 * tsize = msize/BKPERBLK + csize and
	 * msize = csize/CEPERBK
	 */
	h->maddr = CACHE_ADDR + 1;
	m = devsize(cdev) - h->maddr;
	h->csize = ((Devsize)(m-1) * CEPERBK*BKPERBLK) / (CEPERBK*BKPERBLK+1);
	h->msize = h->csize/CEPERBK - 5;
	while(!prime(h->msize))
		h->msize--;
	h->csize = h->msize*CEPERBK;
	h->caddr = h->maddr + (h->msize+BKPERBLK-1)/BKPERBLK;
	h->wsize = devsize(WDEV(dev));

	if(h->msize <= 0)
		panic("cache too small");
	if(h->caddr + h->csize > m)
		panic("cache size error");

	/*
	 * setup cache map
	 */
	for(m=h->maddr; m<h->caddr; m++) {
		p = getbuf(cdev, m, Bmod);
		memset(p->iobuf, 0, RBUFSIZE);
		settag(p, Tbuck, m);
		putbuf(p);
	}
	print("done cacheinit\n");
	return cb;
}

Off
getstartsb(Device *dev)
{
	Filsys *f;
	Startsb *s;

	for(f=filsys; f->name; f++)
		if(devcmpr(f->dev, dev) == 0) {
			for(s=startsb; s->name; s++)
				if(strcmp(f->name, s->name) == 0)
					return s->startsb;
			print("getstartsb: no special starting superblock for %Z %s\n", dev, f->name);
			return FIRST;
		}
	print("getstartsb: no filsys for %Z\n", dev);
	return FIRST;
}

/*
 * ream the cache
 * calculate new buckets
 * get superblock from
 * last worm dump block.
 */
void
cwrecover0(Device *dev)
{
	Iobuf *p, *cb;
	Cache *h;
	Superb *s;
	Off m, l, baddr;
	Device *wdev;

	cwinit1(dev);
	wdev = WDEV(dev);

	p = getbuf(devnone, Cwxx1, 0);
	s = (Superb*)p->iobuf;
	baddr = 0;
	m = getstartsb(dev);
	localconfinit();
	if(conf.firstsb)
		m = conf.firstsb;
	l = m;
	for(;;) {
		memset(p->iobuf, 0, RBUFSIZE);
		if(devread(wdev, m, p->iobuf) ||
		   checktag(p, Tsuper, QPSUPER))
			break;
		l = m;
		baddr = m;
		m = s->next;
		print("dump %lld is good; %lld next\n", (Wideoff)baddr, (Wideoff)m);
		if(baddr == conf.recovsb)
			break;
	}
	if(conf.recovsb == -1){
		baddr = l;
//		memset(p->iobuf, 0, RBUFSIZE);
//		if(devwrite(wdev, m, p->iobuf))
//			panic("erase old sb");
	}
	putbuf(p);
	if(!baddr)
		panic("recover: no superblock\n");

	p = getbuf(wdev, baddr, Bread);
	s = (Superb*)p->iobuf;

	cb = cacheinit(dev);
	h = (Cache*)cb->iobuf;
	h->sbaddr = baddr;
	h->cwraddr = s->cwraddr;
	h->roraddr = s->roraddr;
	h->fsize = s->fsize + 100;		/* this must be conservative */
	if(conf.recovcw)
		h->cwraddr = conf.recovcw;
	if(conf.recovro)
		h->roraddr = conf.recovro;

	putbuf(cb);
	putbuf(p);

	p = getbuf(dev, baddr, Bread|Bmod);
	s = (Superb*)p->iobuf;

	/* TODO: check s->magic byte order, arrange autoswabbing */

	memset(&s->fbuf, 0, sizeof(s->fbuf));
	s->fbuf.free[0] = 0;
	s->fbuf.nfree = 1;
	s->tfree = 0;
	if(conf.recovcw)
		s->cwraddr = conf.recovcw;
	if(conf.recovro)
		s->roraddr = conf.recovro;

	putbuf(p);
	print("done recover\n");
}

void
cwrecover(Device *d)
{
	wlock(&mainlock);	/* recover */
	cwrecover0(d);
	wunlock(&mainlock);
}

/*
 * ream the cache
 * calculate new buckets
 * initialize superblock.
 */
void
cwream0(Device *dev)
{
	Iobuf *p, *cb;
	Cache *h;
	Superb *s;
	Off m, baddr;
	Device *cdev;

	print("cwream %Z\n", dev);
	cwinit1(dev);
	cdev = CDEV(dev);
	devinit(cdev);

	baddr = FIRST;	/*	baddr   = super addr
				baddr+1 = cw root
				baddr+2 = ro root
				baddr+3 = reserved next superblock */

	cb = cacheinit(dev);
	h = (Cache*)cb->iobuf;

	h->sbaddr = baddr;
	h->cwraddr = baddr+1;
	h->roraddr = baddr+2;
	h->fsize = 0;	/* prevents superream from freeing */

	putbuf(cb);

	for(m=0; m<3; m++)
		cwio(dev, baddr+m, 0, Ogrow);
	superream(dev, baddr);
	rootream(dev, baddr+1);			/* cw root */
	rootream(dev, baddr+2);			/* ro root */

	cb = getbuf(cdev, CACHE_ADDR, Bread|Bmod|Bres);
	h = (Cache*)cb->iobuf;
	h->fsize = baddr+4;
	putbuf(cb);

	p = getbuf(dev, baddr, Bread|Bmod|Bimm);
	s = (Superb*)p->iobuf;
	s->last = baddr;
	s->cwraddr = baddr+1;
	s->roraddr = baddr+2;
	s->next = baddr+3;
	s->fsize = baddr+4;
#ifdef AUTOSWAB
	s->magic = 0x123456789abcdef0;
#endif
	putbuf(p);

	for(m=0; m<3; m++)
		cwio(dev, baddr+m, 0, Odump);
}

void
cwream(Device *d, int top)
{
	devream(d->cw.w, 0);
	devream(d->cw.c, 0);
	if(top) {
		wlock(&mainlock);
		cwream0(d);
		wunlock(&mainlock);
	}
	devinit(d);
}

Off
rewalk1(Cw *cw, Off addr, int slot, Wpath *up)
{
	Iobuf *p, *p1;
	Dentry *d;

	if(up == 0)
		return cwraddr(cw->dev);
	up->addr = rewalk1(cw, up->addr, up->slot, up->up);
	p = getbuf(cw->dev, up->addr, Bread|Bmod);
	d = getdir(p, up->slot);
	if(!d || !(d->mode & DALLOC)) {
		print("rewalk1 1\n");
		if(p)
			putbuf(p);
		return addr;
	}
	p1 = dnodebuf(p, d, slot/DIRPERBUF, 0, 0);
	if(!p1) {
		print("rewalk1 2\n");
		if(p)
			putbuf(p);
		return addr;
	}
	dprint("rewalk1 %lld to %lld \"%s\"\n", (Wideoff)addr, (Wideoff)p1->addr, d->name);
	addr = p1->addr;
	p1->flags |= Bmod;
	putbuf(p1);
	putbuf(p);
	return addr;
}

Off
rewalk2(Cw *cw, Off addr, int slot, Wpath *up)
{
	Iobuf *p, *p1;
	Dentry *d;

	if(up == 0)
		return cwraddr(cw->rodev);
	up->addr = rewalk2(cw, up->addr, up->slot, up->up);
	p = getbuf(cw->rodev, up->addr, Bread);
	d = getdir(p, up->slot);
	if(!d || !(d->mode & DALLOC)) {
		print("rewalk2 1\n");
		if(p)
			putbuf(p);
		return addr;
	}
	p1 = dnodebuf(p, d, slot/DIRPERBUF, 0, 0);
	if(!p1) {
		print("rewalk2 2\n");
		if(p)
			putbuf(p);
		return addr;
	}
	dprint("rewalk2 %lld to %lld \"%s\"\n",
			(Wideoff)addr, (Wideoff)p1->addr, d->name);
	addr = p1->addr;
	putbuf(p1);
	putbuf(p);
	return addr;
}

void
rewalk(Cw *cw)
{
	int h;
	File *f;

	for(h=0; h<nelem(flist); h++) {
		for(f=flist[h]; f; f=f->next) {
			if(!f->fs)
				continue;
			if(cw->dev == f->fs->dev)
				f->addr = rewalk1(cw, f->addr, f->slot, f->wpath);
			else
			if(cw->rodev == f->fs->dev)
				f->addr = rewalk2(cw, f->addr, f->slot, f->wpath);
		}
	}
}

Off
split(Cw *cw, Iobuf *p, Off addr)
{
	Off na;
	int state;

	na = 0;
	if(p && (p->flags & Bmod)) {
		dpprint("split: Bmod %lld\n", addr);
		p->flags |= Bimm;
		putbuf(p);
		p = 0;
	}
	state = cwio(cw->dev, addr, 0, Onone);	/* read the state (twice?) */
	switch(state)
	{
	default:
		panic("split: unknown state %s", cwnames[state]);

	case Cerror:
	case Cnone:
	case Cdump:
	case Cread:
		break;

	case Cdump1:
	case Cwrite:
		/*
		 * botch.. could be done by relabeling
		 */
		if(!p) {
			dpprint("split: %s %lld\n", cwnames[state], addr);
			p = getbuf(cw->dev, addr, Bread);
			if(!p) {
				print("split: null getbuf\n");
				break;
			}
		}
		na = cw->fsize;
		cw->fsize = na+1;
		cwio(cw->dev, na, 0, Ogrow);
		cwio(cw->dev, na, p->iobuf, Owrite);
		cwio(cw->dev, na, 0, Odump);
		cwio(cw->dev, addr, 0, Orele);
		break;

	case Cdirty:
		dpprint("split: dirty %lld\n", addr);
		cwio(cw->dev, addr, 0, Odump);
		break;
	}
	if(p)
		putbuf(p);
	return na;
}

int
isdirty(Cw *cw, Iobuf *p, Off addr, int tag)
{
	int s;

	if(p && (p->flags & Bmod))
		return 1;
	s = cwio(cw->dev, addr, 0, Onone);
	if(s == Cdirty || s == Cwrite)
		return 1;
	/*
	 * botch: we should mark the parents of modified
	 * indirect blocks as split-on-dump.
	 */
	if(tag >= Tind1 && tag <= Tmaxind)
		return 2;
	return 0;
}

static char *rtab[] = {
	"not dirty",
	"dirty",
	"dirty?",
};

Off
cwrecur(Cw *cw, Off addr, int tag, int tag1, long qp)
{
	Iobuf *p;
	Dentry *d;
	int i, j, r, shouldstop;
	Off na;
	char *np;

	shouldstop = 0;
	p = getbuf(cw->dev, addr, Bprobe);
	r = isdirty(cw, p, addr, tag);
	dpprint("cwrecur: %lld t=%s %s %s\n", (Wideoff)addr, tagnames[tag], rtab[r], cw->name);
	if(r == 0) {
		if(!cw->all) {
			if(p)
				putbuf(p);
			return 0;
		}
		shouldstop = 1;
	}
	if(cw->depth >= 100) {
		print("dump depth too great %s\n", cw->name);
		if(p)
			putbuf(p);
		return 0;
	}
	cw->depth++;

	switch(tag)
	{
	default:
		print("cwrecur: unknown tag %d %s\n", tag, cw->name);

	case Tfile:
		break;

	case Tsuper:
	case Tdir:
		if(!p) {
			p = getbuf(cw->dev, addr, Bread);
			if(!p) {
				print("cwrecur: Tdir p null %s\n",
					cw->name);
				break;
			}
		}
		if(tag == Tdir) {
			cw->namepad[0] = 0;	/* force room */
			np = strchr(cw->name, 0);
			*np++ = '/';
		} else {
			np = 0;	/* set */
			cw->name[0] = 0;
		}

		for(i=0; i<DIRPERBUF; i++) {
			d = getdir(p, i);
			if(!(d->mode & DALLOC))
				continue;
			qp = d->qid.path & ~QPDIR;
			if(tag == Tdir)
				strncpy(np, d->name, NAMELEN);
			else
			if(i > 0)
				print("cwrecur: root with >1 directory\n");
			tag1 = Tfile;
			if(d->mode & DDIR)
				tag1 = Tdir;
			for(j=0; j<NDBLOCK; j++) {
				na = d->dblock[j];
				if(na) {
					na = cwrecur(cw, na, tag1, 0, qp);
					if(na) {
						d->dblock[j] = na;
						p->flags |= Bmod;
					}
				}
			}
			for (j = 0; j < NIBLOCK; j++) {
				na = d->iblocks[j];
				if(na) {
					na = cwrecur(cw, na, Tind1+j, tag1, qp);
					if(na) {
						d->iblocks[j] = na;
						p->flags |= Bmod;
					}
				}
			}
		}
		break;

	case Tind1:
		j = tag1;
		tag1 = 0;
		goto tind;

	case Tind2:
#ifndef OLD
	case Tind3:
	case Tind4:
#endif
		j = tag-1;
	tind:
		if(!p) {
			p = getbuf(cw->dev, addr, Bread);
			if(!p) {
				print("cwrecur: Tind p null %s\n", cw->name);
				break;
			}
		}
		for(i=0; i<INDPERBUF; i++) {
			na = ((Off *)p->iobuf)[i];
			if(na) {
				na = cwrecur(cw, na, j, tag1, qp);
				if(na) {
					((Off *)p->iobuf)[i] = na;
					p->flags |= Bmod;
				}
			}
		}
		break;
	}
	na = split(cw, p, addr);
	cw->depth--;
	if(na && shouldstop) {
		if(cw->falsehits < 10)
			print("shouldstop %lld %lld t=%s %s\n",
				(Wideoff)addr, (Wideoff)na,
				tagnames[tag], cw->name);
		cw->falsehits++;
	}
	return na;
}

void
cfsdump(Filsys *fs)
{
	long m, n, i;
	Off orba, rba, oroa, roa, sba, a;
	Timet tim;
	char tstr[20];
	Iobuf *pr, *p1, *p;
	Dentry *dr, *d1, *d;
	Cache *h;
	Superb *s;
	Cw *cw;

	if(fs->dev->type != Devcw) {
		print("cant dump: non-cw device: %Z\n", fs->dev);
		return;
	}
	cw = fs->dev->private;
	if(cw == 0) {
		print("cant dump: not inited: %Z\n", fs->dev);
		return;
	}

	tim = toytime();
	wlock(&mainlock);		/* dump */

	/*
	 * set up static structure
	 * with frequent variables
	 */
	cw->ndump = 0;
	cw->name[0] = 0;
	cw->depth = 0;

	/*
	 * cw root
	 */
	sync("before dump");
	cw->fsize = cwsize(cw->dev);
	orba = cwraddr(cw->dev);
	print("cwroot %lld", (Wideoff)orba);
	cons.noage = 1;
	cw->all = cw->allflag;
	rba = cwrecur(cw, orba, Tsuper, 0, QPROOT);
	if(rba == 0)
		rba = orba;
	print("->%lld\n", (Wideoff)rba);
	sync("after cw");

	/*
	 * partial super block
	 */
	p = getbuf(cw->dev, cwsaddr(cw->dev), Bread|Bmod|Bimm);
	s = (Superb*)p->iobuf;
	s->fsize = cw->fsize;
	s->cwraddr = rba;
#ifdef AUTOSWAB
	s->magic = 0x123456789abcdef0;
#endif
	putbuf(p);

	/*
	 * partial cache block
	 */
	p = getbuf(cw->cdev, CACHE_ADDR, Bread|Bmod|Bimm|Bres);
	h = (Cache*)p->iobuf;
	h->fsize = cw->fsize;
	h->cwraddr = rba;
	putbuf(p);

	/*
	 * ro root
	 */
	oroa = cwraddr(cw->rodev);
	pr = getbuf(cw->dev, oroa, Bread|Bmod);
	dr = getdir(pr, 0);

	datestr(tstr, time());	/* tstr = "yyyymmdd" */
	n = 0;
	for(a=0;; a++) {
		p1 = dnodebuf(pr, dr, a, Tdir, 0);
		if(!p1)
			goto bad;
		n++;
		for(i=0; i<DIRPERBUF; i++) {
			d1 = getdir(p1, i);
			if(!d1)
				goto bad;
			if(!(d1->mode & DALLOC))
				goto found1;
			if(!memcmp(d1->name, tstr, 4))
				goto found2;	/* found entry */
		}
		putbuf(p1);
	}

	/*
	 * no year directory, create one
	 */
found1:
	p = getbuf(cw->dev, rba, Bread);
	d = getdir(p, 0);
	d1->qid = d->qid;
	d1->qid.version += n;
	memmove(d1->name, tstr, 4);
	d1->mode = d->mode;
	d1->uid = d->uid;
	d1->gid = d->gid;
	putbuf(p);
	accessdir(p1, d1, FWRITE, 0);

	/*
	 * put mmdd[count] in year directory
	 */
found2:
	accessdir(p1, d1, FREAD, 0);
	putbuf(pr);
	pr = p1;
	dr = d1;

	n = 0;
	m = 0;
	for(a=0;; a++) {
		p1 = dnodebuf(pr, dr, a, Tdir, 0);
		if(!p1)
			goto bad;
		n++;
		for(i=0; i<DIRPERBUF; i++) {
			d1 = getdir(p1, i);
			if(!d1)
				goto bad;
			if(!(d1->mode & DALLOC))
				goto found;
			if(!memcmp(d1->name, tstr+4, 4))
				m++;
		}
		putbuf(p1);
	}

	/*
	 * empty slot put in root
	 */
found:
	if(m)	/* how many dumps this date */
		sprint(tstr+8, "%ld", m);

	p = getbuf(cw->dev, rba, Bread);
	d = getdir(p, 0);
	*d1 = *d;				/* qid is QPROOT */
	putbuf(p);
	strcpy(d1->name, tstr+4);
	d1->qid.version += n;
	accessdir(p1, d1, FWRITE, 0);
	putbuf(p1);
	putbuf(pr);

	cw->fsize = cwsize(cw->dev);
//	oroa = cwraddr(cw->rodev);		/* probably redundant */
	print("roroot %lld\n", (Wideoff)oroa);

	cons.noage = 0;
	cw->all = 0;
	roa = cwrecur(cw, oroa, Tsuper, 0, QPROOT);
	if(roa == 0) {
		print("[same]");
		roa = oroa;
	}
	print("->%lld /%.4s/%s\n", (Wideoff)roa, tstr, tstr+4);
	sync("after ro");

	/*
	 * final super block
	 */
	a = cwsaddr(cw->dev);
	print("sblock %lld", (Wideoff)a);
	p = getbuf(cw->dev, a, Bread|Bmod|Bimm);
	s = (Superb*)p->iobuf;
	s->last = a;
	sba = s->next;
	s->next = cw->fsize;
	cw->fsize++;
	s->fsize = cw->fsize;
	s->roraddr = roa;
#ifdef AUTOSWAB
	s->magic = 0x123456789abcdef0;
#endif

	cwio(cw->dev, sba, 0, Ogrow);
	cwio(cw->dev, sba, p->iobuf, Owrite);
	cwio(cw->dev, sba, 0, Odump);
	print("->%lld (->%lld)\n", (Wideoff)sba, (Wideoff)s->next);

	putbuf(p);

	/*
	 * final cache block
	 */
	p = getbuf(cw->cdev, CACHE_ADDR, Bread|Bmod|Bimm|Bres);
	h = (Cache*)p->iobuf;
	h->fsize = cw->fsize;
	h->roraddr = roa;
	h->sbaddr = sba;
	putbuf(p);

	rewalk(cw);
	sync("all done");

	print("%lld blocks queued for worm\n", (Wideoff)cw->ndump);
	print("%lld falsehits\n", (Wideoff)cw->falsehits);
	cw->nodump = 0;

	/*
	 * extend all of the locks
	 */
	tim = toytime() - tim;
	for(i=0; i<NTLOCK; i++)
		if(tlocks[i].time > 0)
			tlocks[i].time += tim;

	wunlock(&mainlock);
	return;

bad:
	panic("dump: bad");
}

void
mvstates(Device *dev, int s1, int s2, int side)
{
	Iobuf *p, *cb;
	Cache *h;
	Bucket *b;
	Centry *c, *ce;
	Off m, lo, hi, msize, maddr;
	Cw *cw;

	cw = dev->private;
	lo = 0;
	hi = lo + devsize(dev->cw.w);	/* size of all sides totalled */
	if(side >= 0) {
		/* operate on only a single disc side */
		Sidestarts ss;

		wormsidestarts(dev, side, &ss);
		lo = ss.sstart;
		hi = ss.s1start;
	}
	cb = getbuf(cw->cdev, CACHE_ADDR, Bread|Bres);
	if(!cb || checktag(cb, Tcache, QPSUPER))
		panic("cwstats: checktag c bucket");
	h = (Cache*)cb->iobuf;
	msize = h->msize;
	maddr = h->maddr;
	putbuf(cb);

	for(m=0; m<msize; m++) {
		p = getbuf(cw->cdev, maddr + m/BKPERBLK, Bread|Bmod);
		if(!p || checktag(p, Tbuck, maddr + m/BKPERBLK))
			panic("cwtest: checktag c bucket");
		b = (Bucket*)p->iobuf + m%BKPERBLK;
		ce = b->entry + CEPERBK;
		for(c=b->entry; c<ce; c++)
			if(c->state == s1 && c->waddr >= lo && c->waddr < hi)
				c->state = s2;
		putbuf(p);
	}
}

void
prchain(Device *dev, Off m, int flg)
{
	Iobuf *p;
	Superb *s;

	if(m == 0) {
		if(flg)
			m = cwsaddr(dev);
		else
			m = getstartsb(dev);
	}
	p = getbuf(devnone, Cwxx2, 0);
	s = (Superb*)p->iobuf;
	for(;;) {
		memset(p->iobuf, 0, RBUFSIZE);
		if(devread(WDEV(dev), m, p->iobuf) ||
		   checktag(p, Tsuper, QPSUPER))
			break;
		if(flg) {
			print("dump %lld is good; %lld prev\n", (Wideoff)m,
				(Wideoff)s->last);
			print("\t%lld cwroot; %lld roroot\n", (Wideoff)s->cwraddr,
				(Wideoff)s->roraddr);
			if(m <= s->last)
				break;
			m = s->last;
		} else {
			print("dump %lld is good; %lld next\n", (Wideoff)m,
				(Wideoff)s->next);
			print("\t%lld cwroot; %lld roroot\n", (Wideoff)s->cwraddr,
				(Wideoff)s->roraddr);
			if(m >= s->next)
				break;
			m = s->next;
		}
	}
	putbuf(p);
}

void
touchsb(Device *dev)
{
	Iobuf *p;
	Off m;

	m = cwsaddr(dev);
	p = getbuf(devnone, Cwxx2, 0);

	memset(p->iobuf, 0, RBUFSIZE);
	if(devread(WDEV(dev), m, p->iobuf) ||
	   checktag(p, Tsuper, QPSUPER))
		print("%Z block %lld WORM SUPER BLOCK READ FAILED\n",
			WDEV(dev), (Wideoff)m);
	else
		print("%Z touch superblock %lld\n", WDEV(dev), (Wideoff)m);
	putbuf(p);
}

void
storesb(Device *dev, Off last, int doit)
{
	Iobuf *ph, *ps;
	Cache *h;
	Superb *s;
	Off sbaddr, qidgen;

	sbaddr = cwsaddr(dev);

	ps = getbuf(devnone, Cwxx2, 0);
	if(!ps) {
		print("sbstore: getbuf\n");
		return;
	}

	/*
	 * try to read last sb
	 */
	if(devread(WDEV(dev), last, ps->iobuf) ||
	   checktag(ps, Tsuper, QPSUPER))
		print("read last failed\n");
	else
		print("read last succeeded\n");

	s = (Superb*)ps->iobuf;
	qidgen = s->qidgen;
	if(qidgen == 0)
		qidgen = 0x31415;
	qidgen += 1000;
	if(s->next != sbaddr)
		print("next(last) is not sbaddr %lld %lld\n",
			(Wideoff)s->next, (Wideoff)sbaddr);
	else
		print("next(last) is sbaddr\n");

	/*
	 * read cached superblock
	 */
	ph = getbuf(CDEV(dev), CACHE_ADDR, Bread|Bres);
	if(!ph || checktag(ph, Tcache, QPSUPER)) {
		print("cwstats: checktag c bucket\n");
		if(ph)
			putbuf(ph);
		putbuf(ps);
		return;
	} else
		print("read cached sb succeeded\n");

	h = (Cache*)ph->iobuf;

	memset(ps->iobuf, 0, RBUFSIZE);
	settag(ps, Tsuper, QPSUPER);
	ps->flags = 0;
	s = (Superb*)ps->iobuf;

	s->cwraddr = h->cwraddr;
	s->roraddr = h->roraddr;
	s->fsize = h->fsize;
	s->fstart = 2;
	s->last = last;
	s->next = h->roraddr+1;

	s->qidgen = qidgen;
#ifdef AUTOSWAB
	s->magic = 0x123456789abcdef0;
#endif
	putbuf(ph);

	if(s->fsize-1 != s->next ||
	   s->fsize-2 != s->roraddr ||
	   s->fsize-5 != s->cwraddr) {
		print("addrs not in relationship %lld %lld %lld %lld\n",
			(Wideoff)s->cwraddr, (Wideoff)s->roraddr,
			(Wideoff)s->next, (Wideoff)s->fsize);
		putbuf(ps);
		return;
	} else
		print("addresses in relation\n");

	if(doit)
	if(devwrite(WDEV(dev), sbaddr, ps->iobuf))
		print("%Z block %lld WORM SUPER BLOCK WRITE FAILED\n",
			WDEV(dev), (Wideoff)sbaddr);
	ps->flags = 0;
	putbuf(ps);
}

void
savecache(Device *dev)
{
	Iobuf *p, *cb;
	Cache *h;
	Bucket *b;
	Centry *c, *ce;
	long n, left;
	Off m, maddr, msize, *longp, nbyte;
	Device *cdev;

	if(walkto("/adm/cache") || con_open(FID2, OWRITE|OTRUNC)) {
		print("cant open /adm/cache\n");
		return;
	}
	cdev = CDEV(dev);
	cb = getbuf(cdev, CACHE_ADDR, Bread|Bres);
	if(!cb || checktag(cb, Tcache, QPSUPER))
		panic("savecache: checktag c bucket");
	h = (Cache*)cb->iobuf;
	msize = h->msize;
	maddr = h->maddr;
	putbuf(cb);

	n = BUFSIZE;			/* calculate write size */
	if(n > MAXDAT)
		n = MAXDAT;

	cb = getbuf(devnone, Cwxx4, 0);
	longp = (Off *)cb->iobuf;
	left = n/sizeof(Off);
	cons.offset = 0;

	for(m=0; m<msize; m++) {
		if(left < BKPERBLK) {
			nbyte = (n/sizeof(Off) - left) * sizeof(Off);
			con_write(FID2, cb->iobuf, cons.offset, nbyte);
			cons.offset += nbyte;
			longp = (Off *)cb->iobuf;
			left = n/sizeof(Off);
		}
		p = getbuf(cdev, maddr + m/BKPERBLK, Bread);
		if(!p || checktag(p, Tbuck, maddr + m/BKPERBLK))
			panic("cwtest: checktag c bucket");
		b = (Bucket*)p->iobuf + m%BKPERBLK;
		ce = b->entry + CEPERBK;
		for(c = b->entry; c < ce; c++)
			if(c->state == Cread) {
				*longp++ = c->waddr;
				left--;
			}
		putbuf(p);
	}
	nbyte = (n/sizeof(Off) - left) * sizeof(Off);
	con_write(FID2, cb->iobuf, cons.offset, nbyte);
	putbuf(cb);
}

void
loadcache(Device *dev, int dskno)
{
	Iobuf *p, *cb;
	Off m, nbyte, *longp, count;
	Sidestarts ss;

	if(walkto("/adm/cache") || con_open(FID2, OREAD)) {
		print("cant open /adm/cache\n");
		return;
	}

	cb = getbuf(devnone, Cwxx4, 0);
	cons.offset = 0;
	count = 0;

	if (dskno >= 0)
		wormsidestarts(dev, dskno, &ss);
	for(;;) {
		memset(cb->iobuf, 0, BUFSIZE);
		nbyte = con_read(FID2, cb->iobuf, cons.offset, 100) / sizeof(Off);
		if(nbyte <= 0)
			break;
		cons.offset += nbyte * sizeof(Off);
		longp = (Off *)cb->iobuf;
		while(nbyte > 0) {
			m = *longp++;
			nbyte--;
			if(m == 0)
				continue;
			/* if given a diskno, restrict to just that disc side */
			if(dskno < 0 || m >= ss.sstart && m < ss.s1start) {
				p = getbuf(dev, m, Bread);
				if(p)
					putbuf(p);
				count++;
			}
		}
	}
	putbuf(cb);
	print("%lld blocks loaded from worm %d\n", (Wideoff)count, dskno);
}

void
morecache(Device *dev, int dskno, Off size)
{
	Iobuf *p;
	Off m, ml, mh, mm, count;
	Cache *h;
	Sidestarts ss;

	p = getbuf(CDEV(dev), CACHE_ADDR, Bread|Bres);
	if(!p || checktag(p, Tcache, QPSUPER))
		panic("savecache: checktag c bucket");
	h = (Cache*)p->iobuf;
	mm = h->wmax;
	putbuf(p);

	wormsidestarts(dev, dskno, &ss);
	ml = ss.sstart;		/* start at beginning of disc side #dskno */
	mh = ml + size;
	if(mh > mm) {
		mh = mm;
		print("limited to %lld\n", (Wideoff)mh-ml);
	}

	count = 0;
	for(m=ml; m < mh; m++) {
		p = getbuf(dev, m, Bread);
		if(p)
			putbuf(p);
		count++;
	}
	print("%lld blocks loaded from worm %d\n", (Wideoff)count, dskno);
}

void
blockcmp(Device *dev, Off wa, Off ca)
{
	Iobuf *p1, *p2;
	int i, c;

	p1 = getbuf(WDEV(dev), wa, Bread);
	if(!p1) {
		print("blockcmp: wdev error\n");
		return;
	}

	p2 = getbuf(CDEV(dev), ca, Bread);
	if(!p2) {
		print("blockcmp: cdev error\n");
		putbuf(p1);
		return;
	}

	c = 0;
	for(i=0; i<RBUFSIZE; i++)
		if(p1->iobuf[i] != p2->iobuf[i]) {
			print("%4d: %.2x %.2x\n", i, p1->iobuf[i], p2->iobuf[i]);
			c++;
			if(c > 9)
				break;
		}

	if(c == 0)
		print("no error\n");
	putbuf(p1);
	putbuf(p2);
}

void
wblock(Device *dev, Off addr)
{
	Iobuf *p1;
	int i;

	p1 = getbuf(dev, addr, Bread);
	if(p1) {
		i = devwrite(WDEV(dev), addr, p1->iobuf);
		print("i = %d\n", i);
		putbuf(p1);
	}
}

int
convstate(char *name)
{
	int i;

	for(i=0; i<nelem(cwnames); i++)
		if(cwnames[i])
			if(strcmp(cwnames[i], name) == 0)
				return i;
	return -1;
}

void
searchtag(Device *d, Off a, int tag, int n)
{
	Iobuf *p;
	Tag *t;
	int i;

	if(a == 0)
		a = getstartsb(d);
	p = getbuf(devnone, Cwxx2, 0);
	t = (Tag*)(p->iobuf+BUFSIZE);
	for(i=0; i<n; i++) {
		memset(p->iobuf, 0, RBUFSIZE);
		if(devread(WDEV(d), a+i, p->iobuf)) {
			if(n == 1000)
				break;
			continue;
		}
		if(t->tag == tag) {
			print("tag %d found at %Z %lld\n", tag, d, (Wideoff)a+i);
			break;
		}
	}
	putbuf(p);
}

void
cmd_cwcmd(int argc, char *argv[])
{
	Device *dev;
	char *arg;
	char str[28];
	Off s1, s2, a, b, n;
	Cw *cw;

	if(argc <= 1) {
		print("	cwcmd mvstate state1 state2 [platter]\n");
		print("	cwcmd prchain [start] [bakflg]\n");
		print("	cwcmd searchtag [start] [tag] [blocks]\n");
		print("	cwcmd touchsb\n");
		print("	cwcmd savecache\n");
		print("	cwcmd loadcache [dskno]\n");
		print("	cwcmd morecache dskno [count]\n");
		print("	cwcmd blockcmp wbno cbno\n");
		print("	cwcmd startdump [01]\n");
		print("	cwcmd acct\n");
		print("	cwcmd clearacct\n");
		return;
	}
	arg = argv[1];

	/*
	 * items not depend on a cw filesystem
	 */
	if(strcmp(arg, "acct") == 0) {
		for(a=0; a<nelem(growacct); a++) {
			b = growacct[a];
			if(b == 0)
				continue;
			uidtostr(str, a, 1);
			print("%10lld %s\n",((Wideoff)b*ADDFREE*RBUFSIZE+500000)/1000000, str);
		}
		return;
	}
	if(strcmp(arg, "clearacct") == 0) {
		memset(growacct, 0, sizeof(growacct));
		return;
	}

	/*
	 * items depend on cw filesystem
	 */
	dev = cons.curfs->dev;
	if(dev == 0 || dev->type != Devcw || dev->private == 0) {
		print("cfs not a cw filesystem: %Z\n", dev);
		return;
	}
	cw = dev->private;
	if(strcmp(arg, "searchtag") == 0) {
		a = 0;
		if(argc > 2)
			a = number(argv[2], 0, 10);
		b = Tsuper;
		if(argc > 3)
			b = number(argv[3], 0, 10);
		n = 1000;
		if(argc > 4)
			n = number(argv[4], 0, 10);
		searchtag(dev, a, b, n);
	} else if(strcmp(arg, "mvstate") == 0) {
		if(argc < 4)
			goto bad;
		s1 = convstate(argv[2]);
		s2 = convstate(argv[3]);
		if(s1 < 0 || s2 < 0)
			goto bad;
		a = -1;
		if(argc > 4)
			a = number(argv[4], 0, 10);
		mvstates(dev, s1, s2, a);
		return;
	bad:
		print("cwcmd mvstate: bad args\n");
	} else if(strcmp(arg, "prchain") == 0) {
		a = 0;
		if(argc > 2)
			a = number(argv[2], 0, 10);
		s1 = 0;
		if(argc > 3)
			s1 = number(argv[3], 0, 10);
		prchain(dev, a, s1);
	} else if(strcmp(arg, "touchsb") == 0)
		touchsb(dev);
	else if(strcmp(arg, "savecache") == 0)
		savecache(dev);
	else if(strcmp(arg, "loadcache") == 0) {
		s1 = -1;
		if(argc > 2)
			s1 = number(argv[2], 0, 10);
		loadcache(dev, s1);
	} else if(strcmp(arg, "morecache") == 0) {
		if(argc <= 2) {
			print("arg count\n");
			return;
		}
		s1 = number(argv[2], 0, 10);
		if(argc > 3)
			s2 = number(argv[3], 0, 10);
		else
			s2 = wormsizeside(dev, s1); /* default to 1 disc side */
		morecache(dev, s1, s2);
	} else if(strcmp(arg, "blockcmp") == 0) {
		if(argc < 4) {
			print("cannot arg count\n");
			return;
		}
		s1 = number(argv[2], 0, 10);
		s2 = number(argv[3], 0, 10);
		blockcmp(dev, s1, s2);
	} else if(strcmp(arg, "startdump") == 0) {
		if(argc > 2)
			cw->nodump = number(argv[2], 0, 10);
		cw->nodump = !cw->nodump;
		if(cw->nodump)
			print("dump stopped\n");
		else
			print("dump allowed\n");
	} else if(strcmp(arg, "allflag") == 0) {
		if(argc > 2)
			cw->allflag = number(argv[2], 0, 10);
		else
			cw->allflag = !cw->allflag;
		print("allflag = %d; falsehits = %lld\n",
			cw->allflag, (Wideoff)cw->falsehits);
	} else if(strcmp(arg, "storesb") == 0) {
		a = 4168344;
		b = 0;
		if(argc > 2)
			a = number(argv[2], 4168344, 10);
		if(argc > 3)
			b = number(argv[3], 0, 10);
		storesb(dev, a, b);
	}
	else
		print("unknown cwcmd %s\n", arg);
}

/*
httpport.c -- Simple program that uses the HTTP POST command

Created:	Jun 2011 by Philip Homburg for RIPE NCC
*/

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netdb.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "libbb.h"

struct option longopts[]=
{
	{ "delete-file", no_argument, NULL, 'd' },
	{ "post-file", required_argument, NULL, 'p' },
	{ "post-header", required_argument, NULL, 'h' },
	{ "post-footer", required_argument, NULL, 'f' },
	{ NULL, }
};

static char buffer[1024];

static void parse_url(char *url, char **hostp, char **portp, char **hostportp,
	char **pathp);
static void check_result(FILE *tcp_file);
static void eat_headers(FILE *tcp_file, int *chunked, int *content_length);
static int connect_to_name(char *host, char *port);
static void copy_chunked(FILE *in_file, FILE *out_file);
static void copy_bytes(FILE *in_file, FILE *out_file, size_t len);
static void usage(void);
static void fatal(const char *fmt, ...);
static void fatal_err(const char *fmt, ...);
static void write_to_tcp_fd (int fd, FILE *tcp_file);
static void skip_spaces(const char *cp, char **ncp);

int httppost_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
int httppost_main(int argc, char *argv[])
{
	int c,  fd, fdF, fdH, tcp_fd, chunked, content_length;
	int opt_delete_file;
	char *url, *host, *port, *hostport, *path;
	char *post_file, *output_file, *post_footer, *post_header;
	FILE *tcp_file, *out_file;
	struct stat sb, sbF, sbH;
	off_t     cLength;

	post_file= NULL; 
	post_footer=NULL;
	post_header=NULL;
	output_file= NULL;
	opt_delete_file = 0;
	while (c= getopt_long(argc, argv, "O:?", longopts, NULL), c != -1)
	{
		switch(c)
		{
		case 'O':
			output_file= optarg;
			break;
		case 'd':
			opt_delete_file = 1;
			break;
		case 'h':				/* --post-header */
			post_header= optarg;
			break;
		case 'f':				/* --post-footer */
			post_footer= optarg;
			break;

		case 'p':				/* --post-file */
			post_file= optarg;
			break;
		case '?':
			usage();
		default:
			fatal("bad option '%c'", c);
		}
	}

	if (optind != argc-1)
		fatal("exactly one url expected");
	url= argv[optind];

	parse_url(url, &host, &port, &hostport, &path);

	//printf("host: %s\n", host);
	//printf("port: %s\n", port);
	//printf("hostport: %s\n", hostport);
	//printf("path: %s\n", path);

	if(post_header != NULL )
	{	
		fdH = open(post_header, O_RDONLY);
		if(fdH == -1 )
			fatal_err("unable to open header '%s'", post_header);
		if (fstat(fdH, &sbH) == -1)
			fatal_err("fstat failed on header file '%s'", post_header);
		if (!S_ISREG(sbH.st_mode))
			fatal("'%s' header is not a regular file", post_header);
	}

	if(post_footer != NULL )
	{	
		fdF = open(post_footer, O_RDONLY);
		if(fdF == -1 )
			fatal_err("unable to open footer '%s'", post_footer);
		if (fstat(fdF, &sbF) == -1)
			fatal_err("fstat failed on footer file '%s'", post_footer);
		if (!S_ISREG(sbF.st_mode))
			fatal("'%s' footer is not a regular file", post_footer);
	}


	/* Try to open the file before trying to connect */
	if (post_file == NULL)
		fatal("no file to POST");

	fd= open(post_file, O_RDONLY);
	if (fd == -1)
		fatal_err("unable to open '%s'", post_file);
	if (fstat(fd, &sb) == -1)
		fatal_err("fstat failed");
	if (!S_ISREG(sb.st_mode))
		fatal("'%s' is not a regular file", post_file);

	tcp_fd= connect_to_name(host, port);
	if (tcp_fd == -1)
		fatal_err("unable to connect to '%s'", host);

	/* Stdio makes life easy */
	tcp_file= fdopen(tcp_fd, "r+");
	if (tcp_file == NULL)
		fatal("fdopen failed");

	fprintf(tcp_file, "POST %s HTTP/1.1\r\n", path);
	//fprintf(tcp_file, "GET %s HTTP/1.1\r\n", path);
	fprintf(tcp_file, "Host: %s\r\n", host);
	fprintf(tcp_file, "Connection: close\r\n");
	fprintf(tcp_file, "User-Agent: httppost for atlas.ripe.net\r\n");
	fprintf(tcp_file,
			"Content-Type: application/x-www-form-urlencoded\r\n");

	cLength  = sb.st_size;
	if( post_header != NULL )
		cLength  +=  sbH.st_size;

	if( post_footer != NULL )
		cLength  +=  sbF.st_size;


	fprintf(tcp_file, "Content-Length: %lu\r\n", (unsigned long)cLength);
	fprintf(tcp_file, "\r\n");


	if( post_header != NULL )
		 write_to_tcp_fd(fdH, tcp_file); 

	write_to_tcp_fd(fd, tcp_file);

	if( post_footer != NULL)
		write_to_tcp_fd(fdF, tcp_file);

	check_result(tcp_file); 
	eat_headers(tcp_file, &chunked, &content_length);

	if (output_file)
	{
		out_file= fopen(output_file, "w");
		if (!out_file)
			fatal_err("unable to create '%s'", out_file);
	}
	else
		out_file= stdout;

	if (chunked)
	{
		copy_chunked(tcp_file, out_file);
	}
	else if (content_length)
	{
		copy_bytes(tcp_file, out_file, content_length);
	}
	if ( opt_delete_file == 1 )
		unlink (post_file);
	return 0; 
}

static void write_to_tcp_fd (int fd, FILE *tcp_file)
{
	int r;
	/* Copy file */
	while(r= read(fd, buffer, sizeof(buffer)), r > 0)
	{
		if (fwrite(buffer, r, 1, tcp_file) != 1)
			fatal_err("error writing to tcp connection");
	}
	if (r == -1)
		fatal_err("error reading from file");

}


static void parse_url(char *url, char **hostp, char **portp, char **hostportp,
	char **pathp)
{
	char *item;
	const char *cp, *np, *prefix;
	size_t len;

	/* the url must start with 'http://' */
	prefix= "http://";
	len= strlen(prefix);
	if (strncasecmp(prefix, url, len) != 0)
		fatal("bad prefix in url '%s'", url);

	cp= url+len;

	/* Get hostport part */
	np= strchr(cp, '/');
	if (np != NULL)
		len= np-cp;
	else
	{
		len= strlen(cp);
		np= cp+len;
	}
	if (len == 0)
		fatal("missing host part in url '%s'", url);
	item= malloc(len+1);
	if (!item) fatal("out of memory");
	memcpy(item, cp, len);
	item[len]= '\0';
	*hostportp= item;

	/* The remainder is the path */
	cp= np;
	if (cp[0] == '\0')
		cp= "/";
	len= strlen(cp);
	item= malloc(len+1);
	if (!item) fatal("out of memory");
	memcpy(item, cp, len);
	item[len]= '\0';
	*pathp= item;

	/* Extract the host name from hostport */
	cp= *hostportp;
	np= cp;
	if (cp[0] == '[')
	{
		/* IPv6 address literal */
		np= strchr(cp, ']');
		if (np == NULL || np == cp+1)
		{
			fatal("malformed IPv6 address literal in url '%s'",
				url);
		}
	}
	/* Should handle IPv6 address literals */
	np= strchr(np, ':');
	if (np != NULL)
		len= np-cp;
	else
	{
		len= strlen(cp);
		np= cp+len;
	}
	if (len == 0)
		fatal("missing host part in url '%s'", url);
	item= malloc(len+1);
	if (!item) fatal("out of memory");
	if (cp[0] == '[')
	{
		/* Leave out the square brackets */
		memcpy(item, cp+1, len-2);
		item[len-2]= '\0';
	}
	else
	{
		memcpy(item, cp, len);
		item[len]= '\0';
	}
	*hostp= item;

	/* Port */
	cp= np;
	if (cp[0] == '\0')
		cp= "80";
	else
		cp++;
	len= strlen(cp);
	item= malloc(len+1);
	if (!item) fatal("out of memory");
	memcpy(item, cp, len);
	item[len]= '\0';
	*portp= item;
}

static void check_result(FILE *tcp_file)
{
	int major, minor;
	size_t len;
	char *cp, *check, *line;
	const char *prefix;

	if (fgets(buffer, sizeof(buffer), tcp_file) == NULL)
	{
		if (feof(tcp_file))
			fatal("got unexpected EOF from server");
		else
			fatal_err("error reading from server");
	}

	line= buffer;
	cp= strchr(line, '\n');
	if (cp == NULL)
		fatal("line too long");
	cp[0]= '\0';
	if (cp > line && cp[-1] == '\r')
		cp[-1]= '\0';

	/* Check http version */
	prefix= "http/";
	len= strlen(prefix);
	if (strncasecmp(prefix, line, len) != 0)
		fatal("bad prefix in response '%s'", line);
	cp= line+len;
	major= strtoul(cp, &check, 10);
	if (check == cp || check[0] != '.')
		fatal("bad major version in response '%s'", line);
	cp= check+1;
	minor= strtoul(cp, &check, 10);
	if (check == cp || check[0] == '\0' ||
		!isspace(*(unsigned char *)check))
	{
		fatal("bad major version in response '%s'", line);
	}

	skip_spaces(check, &cp);

	if (!isdigit(*(unsigned char *)cp))
		fatal("bad status code in response '%s'", line);

	if (cp[0] != '2')
		fatal("POST command failed: '%s'", cp);
}

static void eat_headers(FILE *tcp_file, int *chunked, int *content_length)
{
	char *line, *cp, *ncp, *check;
	size_t len;
	const char *kw;

	*chunked= 0;
	*content_length= 0;
	while (fgets(buffer, sizeof(buffer), tcp_file) != NULL)
	{
		line= buffer;
		cp= strchr(line, '\n');
		if (cp == NULL)
			fatal("line too long");
		cp[0]= '\0';
		if (cp > line && cp[-1] == '\r')
			cp[-1]= '\0';

		if (line[0] == '\0')
			return;		/* End of headers */

		fprintf(stderr, "httppost: got line '%s'\n", line);

		cp= line;
		skip_spaces(cp, &ncp);
		if (ncp != line)
			continue;	/* Continuation line */

		cp= ncp;
		while (ncp[0] != '\0' && ncp[0] != ':' &&
			!isspace((unsigned char)ncp[0]))
		{
			ncp++;
		}

		kw= "Transfer-Encoding";
		len= strlen(kw);
		if (strncasecmp(cp, kw, len) == 0)
		{
			/* Skip optional white space */
			cp= ncp;
			skip_spaces(cp, &cp);

			if (cp[0] != ':')
				fatal("malformed content-length header", line);
			cp++;

			/* Skip more white space */
			skip_spaces(cp, &cp);

			/* Should have the value by now */
			kw= "chunked";
			len= strlen(kw);
			if (strncasecmp(cp, kw, len) != 0)
				continue;
			/* make sure we have end of line or white space */
			if (cp[len] != '\0' && isspace((unsigned char)cp[len]))
				continue;
			*chunked= 1;
			continue;
		}

		kw= "Content-length";
		len= strlen(kw);
		if (strncasecmp(cp, kw, len) != 0)
			continue;

		/* Skip optional white space */
		cp= ncp;
		skip_spaces(cp, &cp);

		if (cp[0] != ':')
			fatal("malformed content-length header", line);
		cp++;

		/* Skip more white space */
		skip_spaces(cp, &cp);

		/* Should have the value by now */
		*content_length= strtoul(cp, &check, 10);
		if (check == cp)
			fatal("malformed content-length header", line);

		/* And after that we should have just white space */
		cp= check;
		skip_spaces(cp, &cp);

		if (cp[0] != '\0')
			fatal("malformed content-length header", line);
	}
	if (feof(tcp_file))
		fatal("got unexpected EOF from server");
	else
		fatal_err("error reading from server");
}

static int connect_to_name(char *host, char *port)
{
	int r, s, s_errno;
	struct addrinfo *res, *aip;
	struct addrinfo hints;

	memset(&hints, '\0', sizeof(hints));
	hints.ai_socktype= SOCK_STREAM;
	r= getaddrinfo(host, port, &hints, &res);
	if (r != 0)
		fatal("unable to resolve '%s': %s", host, gai_strerror(r));

	s_errno= 0;
	s= -1;
	for (aip= res; aip != NULL; aip= aip->ai_next)
	{
		s= socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if (s == -1)
		{	
			s_errno= errno;
			continue;
		}

		if (connect(s, res->ai_addr, res->ai_addrlen) == 0)
			break;

		s_errno= errno;
		close(s);
		s= -1;
	}

	free(res);
	if (s == -1)
		errno= s_errno;
	return s;
}

static void copy_chunked(FILE *in_file, FILE *out_file)
{
	size_t len, offset, size;
	char *cp, *line, *check;

	for (;;)
	{
		/* Get a chunk size */
		if (fgets(buffer, sizeof(buffer), in_file) == NULL)
			fatal("error reading input");

		line= buffer;
		cp= strchr(line, '\n');
		if (cp == NULL)
			fatal("line too long");
		cp[0]= '\0';
		if (cp > line && cp[-1] == '\r')
			cp[-1]= '\0';

		fprintf(stderr, "httppost: got chunk line '%s'\n", line);
		len= strtoul(line, &check, 16);
		if (check[0] != '\0' && !isspace(*(unsigned char *)check))
			fatal("bad chunk line '%s'", line);
		if (!len)
			break;

		offset= 0;

		while (offset < len)
		{
			size= len-offset;
			if (size > sizeof(buffer))
				size= sizeof(buffer);
			if (fread(buffer, size, 1, in_file) != 1)
				fatal_err("error reading input");
			if (fwrite(buffer, size, 1, out_file) != 1)
				fatal_err("error writing output");
			offset += size;
		}

		/* Expect empty line after data */
		if (fgets(buffer, sizeof(buffer), in_file) == NULL)
			fatal("error reading input");

		line= buffer;
		cp= strchr(line, '\n');
		if (cp == NULL)
			fatal("line too long");
		cp[0]= '\0';
		if (cp > line && cp[-1] == '\r')
			cp[-1]= '\0';
		if (line[0] != '\0')
			fatal("Garbage after chunk data");
	}

	for (;;)
	{
		/* Get an end-of-chunk line */
		if (fgets(buffer, sizeof(buffer), in_file) == NULL)
			fatal("error reading input");

		line= buffer;
		cp= strchr(line, '\n');
		if (cp == NULL)
			fatal("line too long");
		cp[0]= '\0';
		if (cp > line && cp[-1] == '\r')
			cp[-1]= '\0';
		if (line[0] == '\0')
			break;

		fprintf(stderr, "httppost: got end-of-chunk line '%s'\n", line);
	}
}
static void copy_bytes(FILE *in_file, FILE *out_file, size_t len)
{
	size_t offset, size;

	offset= 0;

	while (offset < len)
	{
		size= len-offset;
		if (size > sizeof(buffer))
			size= sizeof(buffer);
		if (fread(buffer, size, 1, in_file) != 1)
			fatal_err("error reading input");
		if (fwrite(buffer, size, 1, out_file) != 1)
			fatal_err("error writing output");
		offset += size;
	}
}

static void skip_spaces(const char *cp, char **ncp)
{
	const unsigned char *ucp;

	ucp= (const unsigned char *)cp;
	while (ucp[0] != '\0' && isspace(ucp[0]))
		ucp++;
	*ncp= (char *)ucp;
}

static void usage(void)
{
	fprintf(stderr,
"Usage: httppost\n"); 
	fprintf(stderr,
"         [--post-header <file-to-post>] [--post-file <file-to-post>]\n");
	fprintf(stderr, 
"        [--post-footer  <file-to-post>] \n");

	fprintf(stderr, 
"        [--delete-file 'delete the upon success, not header and footer'\n");

	fprintf(stderr, 
"        [--post-footer  <file-to-post>] [-O <output-file>] <url>\n");
	exit(1);
}

static void fatal(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);

	fprintf(stderr, "httppost: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");

	va_end(ap);

	exit(1);
}

static void fatal_err(const char *fmt, ...)
{
	int s_errno;
	va_list ap;

	s_errno= errno;

	va_start(ap, fmt);

	fprintf(stderr, "httppost: ");
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, ": %s\n", strerror(s_errno));

	va_end(ap);

	exit(1);
}

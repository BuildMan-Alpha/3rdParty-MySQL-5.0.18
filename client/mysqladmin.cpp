/* Copyright (C) 2000-2004 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

/* maintaince of mysql databases */

#include "client_priv.h"
#include <signal.h>
#ifdef THREAD
#include <my_pthread.h>				/* because of signal()	*/
#endif
#include <sys/stat.h>
#include <mysql.h>

#ifdef LATER_HAVE_NDBCLUSTER_DB
#include "../ndb/src/mgmclient/ndb_mgmclient.h"
#endif

#define ADMIN_VERSION "8.41"
#define MAX_MYSQL_VAR 256
#define SHUTDOWN_DEF_TIMEOUT 3600		/* Wait for shutdown */
#define MAX_TRUNC_LENGTH 3

char *host= NULL, *user= 0, *opt_password= 0,
     *default_charset= NULL;
char truncated_var_names[MAX_MYSQL_VAR][MAX_TRUNC_LENGTH];
char ex_var_names[MAX_MYSQL_VAR][FN_REFLEN];
ulonglong last_values[MAX_MYSQL_VAR];
static int interval=0;
static my_bool option_force=0,interrupted=0,new_line=0,
               opt_compress=0, opt_relative=0, opt_verbose=0, opt_vertical=0,
               tty_password=0;
static uint tcp_port = 0, option_wait = 0, option_silent=0, nr_iterations,
            opt_count_iterations= 0;
static ulong opt_connect_timeout, opt_shutdown_timeout;
static my_string unix_port=0;
#ifdef LATER_HAVE_NDBCLUSTER_DB
static my_bool opt_ndbcluster=0;
static char *opt_ndb_connectstring=0;
#endif

#ifdef HAVE_SMEM
static char *shared_memory_base_name=0;
#endif
static uint opt_protocol=0;

/*
  When using extended-status relatively, ex_val_max_len is the estimated
  maximum length for any relative value printed by extended-status. The
  idea is to try to keep the length of output as short as possible.
*/

static uint ex_val_max_len[MAX_MYSQL_VAR];
static my_bool ex_status_printed = 0; /* First output is not relative. */
static uint ex_var_count, max_var_length, max_val_length;

#include <sslopt-vars.h>

static void print_version(void);
static void usage(void);
static my_bool sql_connect(MYSQL *mysql, uint wait);
static int execute_commands(MYSQL *mysql,int argc, char **argv);
static int drop_db(MYSQL *mysql,const char *db);
static sig_handler endprog(int signal_number);
static void nice_time(ulong sec,char *buff);
static void print_header(MYSQL_RES *result);
static void print_top(MYSQL_RES *result);
static void print_row(MYSQL_RES *result,MYSQL_ROW cur, uint row);
static void print_relative_row(MYSQL_RES *result, MYSQL_ROW cur, uint row);
static void print_relative_row_vert(MYSQL_RES *result, MYSQL_ROW cur, uint row);
static void print_relative_header();
static void print_relative_line();
static void truncate_names();
static my_bool get_pidfile(MYSQL *mysql, char *pidfile);
static my_bool wait_pidfile(char *pidfile, time_t last_modified,
			    struct stat *pidfile_status);
static void store_values(MYSQL_RES *result);

/*
  The order of commands must be the same as command_names,
  except ADMIN_ERROR
*/
enum commands {
  ADMIN_ERROR,
  ADMIN_CREATE,           ADMIN_DROP,            ADMIN_SHUTDOWN,
  ADMIN_RELOAD,           ADMIN_REFRESH,         ADMIN_VER,
  ADMIN_PROCESSLIST,      ADMIN_STATUS,          ADMIN_KILL,
  ADMIN_DEBUG,            ADMIN_VARIABLES,       ADMIN_FLUSH_LOGS,
  ADMIN_FLUSH_HOSTS,      ADMIN_FLUSH_TABLES,    ADMIN_PASSWORD,
  ADMIN_PING,             ADMIN_EXTENDED_STATUS, ADMIN_FLUSH_STATUS,
  ADMIN_FLUSH_PRIVILEGES, ADMIN_START_SLAVE,     ADMIN_STOP_SLAVE,
  ADMIN_FLUSH_THREADS,    ADMIN_OLD_PASSWORD
#ifdef LATER_HAVE_NDBCLUSTER_DB
  ,ADMIN_NDB_MGM
#endif
};
static const char *command_names[]= {
  "create",               "drop",                "shutdown",
  "reload",               "refresh",             "version",
  "processlist",          "status",              "kill",
  "debug",                "variables",           "flush-logs",
  "flush-hosts",          "flush-tables",        "password",
  "ping",                 "extended-status",     "flush-status",
  "flush-privileges",     "start-slave",         "stop-slave",
  "flush-threads","old-password",
#ifdef LATER_HAVE_NDBCLUSTER_DB
  "ndb-mgm",
#endif
  NullS
};

static TYPELIB command_typelib=
{ array_elements(command_names)-1,"commands", command_names, NULL};

static struct my_option my_long_options[] =
{
#ifdef __NETWARE__
  {"autoclose", OPT_AUTO_CLOSE, "Auto close the screen on exit for Netware.",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
#endif
  {"count", 'c',
   "Number of iterations to make. This works with -i (--sleep) only.",
   (gptr*) &nr_iterations, (gptr*) &nr_iterations, 0, GET_UINT,
   REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"debug", '#', "Output debug log. Often this is 'd:t:o,filename'.",
   0, 0, 0, GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0},
  {"force", 'f',
   "Don't ask for confirmation on drop database; with multiple commands, continue even if an error occurs.",
   (gptr*) &option_force, (gptr*) &option_force, 0, GET_BOOL, NO_ARG, 0, 0,
   0, 0, 0, 0},
  {"compress", 'C', "Use compression in server/client protocol.",
   (gptr*) &opt_compress, (gptr*) &opt_compress, 0, GET_BOOL, NO_ARG, 0, 0, 0,
   0, 0, 0},
  {"character-sets-dir", OPT_CHARSETS_DIR,
   "Directory where character sets are.", (gptr*) &charsets_dir,
   (gptr*) &charsets_dir, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"default-character-set", OPT_DEFAULT_CHARSET,
   "Set the default character set.", (gptr*) &default_charset,
   (gptr*) &default_charset, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"help", '?', "Display this help and exit.", 0, 0, 0, GET_NO_ARG,
   NO_ARG, 0, 0, 0, 0, 0, 0},
  {"host", 'h', "Connect to host.", (gptr*) &host, (gptr*) &host, 0, GET_STR,
   REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"password", 'p',
   "Password to use when connecting to server. If password is not given it's asked from the tty.",
   0, 0, 0, GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0},
#ifdef __WIN__
  {"pipe", 'W', "Use named pipes to connect to server.", 0, 0, 0, GET_NO_ARG,
   NO_ARG, 0, 0, 0, 0, 0, 0},
#endif
  {"port", 'P', "Port number to use for connection.", (gptr*) &tcp_port,
   (gptr*) &tcp_port, 0, GET_UINT, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"protocol", OPT_MYSQL_PROTOCOL, "The protocol of connection (tcp,socket,pipe,memory).",
    0, 0, 0, GET_STR,  REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"relative", 'r',
   "Show difference between current and previous values when used with -i. Currently works only with extended-status.",
   (gptr*) &opt_relative, (gptr*) &opt_relative, 0, GET_BOOL, NO_ARG, 0, 0, 0,
  0, 0, 0},
  {"set-variable", 'O',
   "Change the value of a variable. Please note that this option is deprecated; you can set variables directly with --variable-name=value.",
   0, 0, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
#ifdef HAVE_SMEM
  {"shared-memory-base-name", OPT_SHARED_MEMORY_BASE_NAME,
   "Base name of shared memory.", (gptr*) &shared_memory_base_name, (gptr*) &shared_memory_base_name,
   0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
#endif
  {"silent", 's', "Silently exit if one can't connect to server.",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"socket", 'S', "Socket file to use for connection.",
   (gptr*) &unix_port, (gptr*) &unix_port, 0, GET_STR, REQUIRED_ARG, 0, 0, 0,
   0, 0, 0},
  {"sleep", 'i', "Execute commands again and again with a sleep between.",
   (gptr*) &interval, (gptr*) &interval, 0, GET_INT, REQUIRED_ARG, 0, 0, 0, 0,
   0, 0},
#include <sslopt-longopts.h>
#ifndef DONT_ALLOW_USER_CHANGE
  {"user", 'u', "User for login if not current user.", (gptr*) &user,
   (gptr*) &user, 0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
#endif
  {"verbose", 'v', "Write more information.", (gptr*) &opt_verbose,
   (gptr*) &opt_verbose, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"version", 'V', "Output version information and exit.", 0, 0, 0, GET_NO_ARG,
   NO_ARG, 0, 0, 0, 0, 0, 0},
  {"vertical", 'E',
   "Print output vertically. Is similar to --relative, but prints output vertically.",
   (gptr*) &opt_vertical, (gptr*) &opt_vertical, 0, GET_BOOL, NO_ARG, 0, 0, 0,
   0, 0, 0},
  {"wait", 'w', "Wait and retry if connection is down.", 0, 0, 0, GET_UINT,
   OPT_ARG, 0, 0, 0, 0, 0, 0},
  {"connect_timeout", OPT_CONNECT_TIMEOUT, "", (gptr*) &opt_connect_timeout,
   (gptr*) &opt_connect_timeout, 0, GET_ULONG, REQUIRED_ARG, 3600*12, 0,
   3600*12, 0, 1, 0},
  {"shutdown_timeout", OPT_SHUTDOWN_TIMEOUT, "", (gptr*) &opt_shutdown_timeout,
   (gptr*) &opt_shutdown_timeout, 0, GET_ULONG, REQUIRED_ARG,
   SHUTDOWN_DEF_TIMEOUT, 0, 3600*12, 0, 1, 0},
#ifdef LATER_HAVE_NDBCLUSTER_DB
  {"ndbcluster", OPT_NDBCLUSTER, ""
   "", (gptr*) &opt_ndbcluster,
   (gptr*) &opt_ndbcluster, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"ndb-connectstring", OPT_NDB_CONNECTSTRING, ""
   "", (gptr*) &opt_ndb_connectstring,
   (gptr*) &opt_ndb_connectstring, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
#endif
  { 0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
};


static const char *load_default_groups[]= { "mysqladmin","client",0 };

static my_bool
get_one_option(int optid, const struct my_option *opt __attribute__((unused)),
	       char *argument)
{
  int error = 0;

  switch(optid) {
#ifdef __NETWARE__
  case OPT_AUTO_CLOSE:
    setscreenmode(SCR_AUTOCLOSE_ON_EXIT);
    break;
#endif
  case 'c':
    opt_count_iterations= 1;
    break;
  case 'p':
    if (argument)
    {
      char *start=argument;
      my_free(opt_password,MYF(MY_ALLOW_ZERO_PTR));
      opt_password=my_strdup(argument,MYF(MY_FAE));
      while (*argument) *argument++= 'x';		/* Destroy argument */
      if (*start)
	start[1]=0;				/* Cut length of argument */
      tty_password= 0;
    }
    else
      tty_password=1;
    break;
  case 's':
    option_silent++;
    break;
  case 'W':
#ifdef __WIN__
    opt_protocol = MYSQL_PROTOCOL_PIPE;
#endif
    break;
  case '#':
    DBUG_PUSH(argument ? argument : "d:t:o,/tmp/mysqladmin.trace");
    break;
#include <sslopt-case.h>
  case 'V':
    print_version();
    exit(0);
    break;
  case 'w':
    if (argument)
    {
      if ((option_wait=atoi(argument)) <= 0)
	option_wait=1;
    }
    else
      option_wait= ~(uint)0;
    break;
  case '?':
  case 'I':					/* Info */
    error++;
    break;
  case OPT_CHARSETS_DIR:
#if MYSQL_VERSION_ID > 32300
    charsets_dir = argument;
#endif
    break;
  case OPT_MYSQL_PROTOCOL:
  {
    if ((opt_protocol= find_type(argument, &sql_protocol_typelib,0)) <= 0)
    {
      fprintf(stderr, "Unknown option to protocol: %s\n", argument);
      exit(1);
    }
    break;
  }
  }
  if (error)
  {
    usage();
    exit(1);
  }
  return 0;
}


int main(int argc,char *argv[])
{
  int error= 0, ho_error;
  MYSQL mysql;
  char **commands, **save_argv;

  MY_INIT(argv[0]);
  mysql_init(&mysql);
  load_defaults("my",load_default_groups,&argc,&argv);
  save_argv = argv;				/* Save for free_defaults */
  if ((ho_error=handle_options(&argc, &argv, my_long_options, get_one_option)))
  {
    free_defaults(save_argv);
    exit(ho_error);
  }

  if (argc == 0)
  {
    usage();
    exit(1);
  }
  commands = argv;
  if (tty_password)
    opt_password = get_tty_password(NullS);

  VOID(signal(SIGINT,endprog));			/* Here if abort */
  VOID(signal(SIGTERM,endprog));		/* Here if abort */

  if (opt_compress)
    mysql_options(&mysql,MYSQL_OPT_COMPRESS,NullS);
  if (opt_connect_timeout)
  {
    uint tmp=opt_connect_timeout;
    mysql_options(&mysql,MYSQL_OPT_CONNECT_TIMEOUT, (char*) &tmp);
  }
#ifdef HAVE_OPENSSL
  if (opt_use_ssl)
    mysql_ssl_set(&mysql, opt_ssl_key, opt_ssl_cert, opt_ssl_ca,
		  opt_ssl_capath, opt_ssl_cipher);
#endif
  if (opt_protocol)
    mysql_options(&mysql,MYSQL_OPT_PROTOCOL,(char*)&opt_protocol);
#ifdef HAVE_SMEM
  if (shared_memory_base_name)
    mysql_options(&mysql,MYSQL_SHARED_MEMORY_BASE_NAME,shared_memory_base_name);
#endif
  if (default_charset)
    mysql_options(&mysql, MYSQL_SET_CHARSET_NAME, default_charset);
  if (sql_connect(&mysql, option_wait))
  {
    unsigned int err= mysql_errno(&mysql);
    if (err >= CR_MIN_ERROR && err <= CR_MAX_ERROR)
      error= 1;
    else
    {
      /* Return 0 if all commands are PING */
      for (; argc > 0; argv++, argc--)
      {
        if (find_type(argv[0], &command_typelib, 2) != ADMIN_PING)
        {
          error= 1;
          break;
        }
      }
    }
  }
  else
  {
    while (!interrupted && (!opt_count_iterations || nr_iterations))
    {
      new_line = 0;
      if ((error=execute_commands(&mysql,argc,commands)))
      {
	if (error > 0)
	  break;				/* Wrong command error */
	if (!option_force)
	{
	  if (option_wait && !interrupted)
	  {
	    mysql_close(&mysql);
	    if (!sql_connect(&mysql, option_wait))
	    {
	      sleep(1);				/* Don't retry too rapidly */
	      continue;				/* Retry */
	    }
	  }
	  error=1;
	  break;
	}
      }
      if (interval)
      {
	sleep(interval);
	if (new_line)
	  puts("");
	if (opt_count_iterations)
	  nr_iterations--;
      }
      else
	break;
    }
    mysql_close(&mysql);
  }
  my_free(opt_password,MYF(MY_ALLOW_ZERO_PTR));
  my_free(user,MYF(MY_ALLOW_ZERO_PTR));
#ifdef HAVE_SMEM
  my_free(shared_memory_base_name,MYF(MY_ALLOW_ZERO_PTR));
#endif
  free_defaults(save_argv);
  my_end(0);
  exit(error ? 1 : 0);
  return 0;
}


static sig_handler endprog(int signal_number __attribute__((unused)))
{
  interrupted=1;
}


static my_bool sql_connect(MYSQL *mysql, uint wait)
{
  my_bool info=0;

  for (;;)
  {
    if (mysql_real_connect(mysql,host,user,opt_password,NullS,tcp_port,
			   unix_port, 0))
    {
      mysql->reconnect= 1;
      if (info)
      {
	fputs("\n",stderr);
	(void) fflush(stderr);
      }
      return 0;
    }

    if (!wait)
    {
      if (!option_silent)
      {
	if (!host)
	  host= (char*) LOCAL_HOST;
	my_printf_error(0,"connect to server at '%s' failed\nerror: '%s'",
			MYF(ME_BELL), host, mysql_error(mysql));
	if (mysql_errno(mysql) == CR_CONNECTION_ERROR)
	{
	  fprintf(stderr,
		  "Check that mysqld is running and that the socket: '%s' exists!\n",
		  unix_port ? unix_port : mysql_unix_port);
	}
	else if (mysql_errno(mysql) == CR_CONN_HOST_ERROR ||
		 mysql_errno(mysql) == CR_UNKNOWN_HOST)
	{
	  fprintf(stderr,"Check that mysqld is running on %s",host);
	  fprintf(stderr," and that the port is %d.\n",
		  tcp_port ? tcp_port: mysql_port);
	  fprintf(stderr,"You can check this by doing 'telnet %s %d'\n",
		  host, tcp_port ? tcp_port: mysql_port);
	}
      }
      return 1;
    }
    if (wait != (uint) ~0)
      wait--;				/* One less retry */
    if ((mysql_errno(mysql) != CR_CONN_HOST_ERROR) &&
	(mysql_errno(mysql) != CR_CONNECTION_ERROR))
    {
      fprintf(stderr,"Got error: %s\n", mysql_error(mysql));
      if (!option_force)
	return 1;
    }
    else if (!option_silent)
    {
      if (!info)
      {
	info=1;
	fputs("Waiting for MySQL server to answer",stderr);
	(void) fflush(stderr);
      }
      else
      {
	putc('.',stderr);
	(void) fflush(stderr);
      }
    }
    sleep(5);
  }
}


/*
  Execute a command.
  Return 0 on ok
	 -1 on retryable error
	 1 on fatal error
*/

static int execute_commands(MYSQL *mysql,int argc, char **argv)
{
  const char *status;
  /*
    MySQL documentation relies on the fact that mysqladmin will
    execute commands in the order specified, e.g.
    mysqladmin -u root flush-privileges password "newpassword"
    to reset a lost root password.
    If this behaviour is ever changed, Docs should be notified.
  */

  struct rand_struct rand_st;

  for (; argc > 0 ; argv++,argc--)
  {
    switch (find_type(argv[0],&command_typelib,2)) {
    case ADMIN_CREATE:
    {
      char buff[FN_REFLEN+20];
      if (argc < 2)
      {
	my_printf_error(0,"Too few arguments to create",MYF(ME_BELL));
	return 1;
      }
      sprintf(buff,"create database `%.*s`",FN_REFLEN,argv[1]);
      if (mysql_query(mysql,buff))
      {
	my_printf_error(0,"CREATE DATABASE failed; error: '%-.200s'",
			MYF(ME_BELL), mysql_error(mysql));
	return -1;
      }
      argc--; argv++;
      break;
    }
    case ADMIN_DROP:
    {
      if (argc < 2)
      {
	my_printf_error(0,"Too few arguments to drop",MYF(ME_BELL));
	return 1;
      }
      if (drop_db(mysql,argv[1]))
	return -1;
      argc--; argv++;
      break;
    }
    case ADMIN_SHUTDOWN:
    {
      char pidfile[FN_REFLEN];
      my_bool got_pidfile= 0;
      time_t last_modified= 0;
      struct stat pidfile_status;

      /*
	Only wait for pidfile on local connections
	If pidfile doesn't exist, continue without pid file checking
      */
      if (mysql->unix_socket && (got_pidfile= !get_pidfile(mysql, pidfile)) &&
	  !stat(pidfile, &pidfile_status))
	last_modified= pidfile_status.st_mtime;

      if (mysql_shutdown(mysql, SHUTDOWN_DEFAULT))
      {
	my_printf_error(0,"shutdown failed; error: '%s'",MYF(ME_BELL),
			mysql_error(mysql));
	return -1;
      }
      mysql_close(mysql);	/* Close connection to avoid error messages */
      if (got_pidfile)
      {
	if (opt_verbose)
	  printf("Shutdown signal sent to server;  Waiting for pid file to disappear\n");

	/* Wait until pid file is gone */
	if (wait_pidfile(pidfile, last_modified, &pidfile_status))
	  return -1;
      }
      break;
    }
    case ADMIN_FLUSH_PRIVILEGES:
    case ADMIN_RELOAD:
      if (mysql_query(mysql,"flush privileges"))
      {
	my_printf_error(0,"reload failed; error: '%s'",MYF(ME_BELL),
			mysql_error(mysql));
	return -1;
      }
      break;
    case ADMIN_REFRESH:
      if (mysql_refresh(mysql,
			(uint) ~(REFRESH_GRANT | REFRESH_STATUS |
				 REFRESH_READ_LOCK | REFRESH_SLAVE |
				 REFRESH_MASTER)))
      {
	my_printf_error(0,"refresh failed; error: '%s'",MYF(ME_BELL),
			mysql_error(mysql));
	return -1;
      }
      break;
    case ADMIN_FLUSH_THREADS:
      if (mysql_refresh(mysql,(uint) REFRESH_THREADS))
      {
	my_printf_error(0,"refresh failed; error: '%s'",MYF(ME_BELL),
			mysql_error(mysql));
	return -1;
      }
      break;
    case ADMIN_VER:
      new_line=1;
      print_version();
      puts("Copyright (C) 2000 MySQL AB & MySQL Finland AB & TCX DataKonsult AB");
      puts("This software comes with ABSOLUTELY NO WARRANTY. This is free software,\nand you are welcome to modify and redistribute it under the GPL license\n");
      printf("Server version\t\t%s\n", mysql_get_server_info(mysql));
      printf("Protocol version\t%d\n", mysql_get_proto_info(mysql));
      printf("Connection\t\t%s\n",mysql_get_host_info(mysql));
      if (mysql->unix_socket)
	printf("UNIX socket\t\t%s\n", mysql->unix_socket);
      else
	printf("TCP port\t\t%d\n", mysql->port);
      status=mysql_stat(mysql);
      {
	char *pos,buff[40];
	ulong sec;
	pos= (char*) strchr(status,' ');
	*pos++=0;
	printf("%s\t\t\t",status);			/* print label */
	if ((status=str2int(pos,10,0,LONG_MAX,(long*) &sec)))
	{
	  nice_time(sec,buff);
	  puts(buff);				/* print nice time */
	  while (*status == ' ') status++;	/* to next info */
	}
      }
      putc('\n',stdout);
      if (status)
	puts(status);
      break;
    case ADMIN_PROCESSLIST:
    {
      MYSQL_RES *result;
      MYSQL_ROW row;

      if (mysql_query(mysql, (opt_verbose ? "show full processlist" :
			      "show processlist")) ||
	  !(result = mysql_store_result(mysql)))
      {
	my_printf_error(0,"process list failed; error: '%s'",MYF(ME_BELL),
			mysql_error(mysql));
	return -1;
      }
      print_header(result);
      while ((row=mysql_fetch_row(result)))
	print_row(result,row,0);
      print_top(result);
      mysql_free_result(result);
      new_line=1;
      break;
    }
    case ADMIN_STATUS:
      status=mysql_stat(mysql);
      if (status)
	puts(status);
      break;
    case ADMIN_KILL:
      {
	uint error=0;
	char *pos;
	if (argc < 2)
	{
	  my_printf_error(0,"Too few arguments to 'kill'",MYF(ME_BELL));
	  return 1;
	}
	pos=argv[1];
	for (;;)
	{
	  if (mysql_kill(mysql,(ulong) atol(pos)))
	  {
	    my_printf_error(0,"kill failed on %ld; error: '%s'",MYF(ME_BELL),
			    atol(pos), mysql_error(mysql));
	    error=1;
	  }
	  if (!(pos=strchr(pos,',')))
	    break;
	  pos++;
	}
	argc--; argv++;
	if (error)
	  return -1;
	break;
      }
    case ADMIN_DEBUG:
      if (mysql_dump_debug_info(mysql))
      {
	my_printf_error(0,"debug failed; error: '%s'",MYF(ME_BELL),
			mysql_error(mysql));
	return -1;
      }
      break;
    case ADMIN_VARIABLES:
    {
      MYSQL_RES *res;
      MYSQL_ROW row;

      new_line=1;
      if (mysql_query(mysql,"show /*!40003 GLOBAL */ variables") ||
	  !(res=mysql_store_result(mysql)))
      {
	my_printf_error(0,"unable to show variables; error: '%s'",MYF(ME_BELL),
			mysql_error(mysql));
	return -1;
      }
      print_header(res);
      while ((row=mysql_fetch_row(res)))
	print_row(res,row,0);
      print_top(res);
      mysql_free_result(res);
      break;
    }
    case ADMIN_EXTENDED_STATUS:
    {
      MYSQL_RES *res;
      MYSQL_ROW row;
      uint rownr = 0;
      void (*func) (MYSQL_RES*, MYSQL_ROW, uint);

      new_line = 1;
      if (mysql_query(mysql, "show /*!50002 GLOBAL */ status") ||
	  !(res = mysql_store_result(mysql)))
      {
	my_printf_error(0, "unable to show status; error: '%s'", MYF(ME_BELL),
			mysql_error(mysql));
	return -1;
      }
      if (!opt_vertical)
	print_header(res);
      else
      {
	if (!ex_status_printed)
	{
	  store_values(res);
	  truncate_names();   /* Does some printing also */
	}
	else
	{
	  print_relative_line();
	  print_relative_header();
	  print_relative_line();
	}
      }

      /*      void (*func) (MYSQL_RES*, MYSQL_ROW, uint); */
      if (opt_relative && !opt_vertical)
	func = print_relative_row;
      else if (opt_vertical)
	func = print_relative_row_vert;
      else
	func = print_row;

      while ((row = mysql_fetch_row(res)))
	(*func)(res, row, rownr++);
      if (opt_vertical)
      {
	if (ex_status_printed)
	{
	  putchar('\n');
	  print_relative_line();
	}
      }
      else
	print_top(res);

      ex_status_printed = 1; /* From now on the output will be relative */
      mysql_free_result(res);
      break;
    }
    case ADMIN_FLUSH_LOGS:
    {
      if (mysql_refresh(mysql,REFRESH_LOG))
      {
	my_printf_error(0,"refresh failed; error: '%s'",MYF(ME_BELL),
			mysql_error(mysql));
	return -1;
      }
      break;
    }
    case ADMIN_FLUSH_HOSTS:
    {
      if (mysql_query(mysql,"flush hosts"))
      {
	my_printf_error(0,"refresh failed; error: '%s'",MYF(ME_BELL),
			mysql_error(mysql));
	return -1;
      }
      break;
    }
    case ADMIN_FLUSH_TABLES:
    {
      if (mysql_query(mysql,"flush tables"))
      {
	my_printf_error(0,"refresh failed; error: '%s'",MYF(ME_BELL),
			mysql_error(mysql));
	return -1;
      }
      break;
    }
    case ADMIN_FLUSH_STATUS:
    {
      if (mysql_query(mysql,"flush status"))
      {
	my_printf_error(0,"refresh failed; error: '%s'",MYF(ME_BELL),
			mysql_error(mysql));
	return -1;
      }
      break;
    }
    case ADMIN_OLD_PASSWORD:
    case ADMIN_PASSWORD:
    {
      char buff[128],crypted_pw[64];
      time_t start_time;
      /* Do initialization the same way as we do in mysqld */
      start_time=time((time_t*) 0);
      randominit(&rand_st,(ulong) start_time,(ulong) start_time/2);

      if (argc < 2)
      {
	my_printf_error(0,"Too few arguments to change password",MYF(ME_BELL));
	return 1;
      }
      if (argv[1][0])
      {
        char *pw= argv[1];
        bool old= (find_type(argv[0], &command_typelib, 2) ==
                   ADMIN_OLD_PASSWORD);
#ifdef __WIN__
        uint pw_len= strlen(pw);
        if (pw_len > 1 && pw[0] == '\'' && pw[pw_len-1] == '\'')
          printf("Warning: single quotes were not trimmed from the password by"
                 " your command\nline client, as you might have expected.\n");
#endif
        /*
           If we don't already know to use an old-style password, see what
           the server is using
        */
        if (!old)
        {
          if (mysql_query(mysql, "SHOW VARIABLES LIKE 'old_passwords'"))
          {
            my_printf_error(0, "Could not determine old_passwords setting from server; error: '%s'",
                	    MYF(ME_BELL),mysql_error(mysql));
            return -1;
          }
          else
          {
            MYSQL_RES *res= mysql_store_result(mysql);
            if (!res)
            {
              my_printf_error(0,
                              "Could not get old_passwords setting from "
                              "server; error: '%s'",
        		      MYF(ME_BELL),mysql_error(mysql));
              return -1;
            }
            if (!mysql_num_rows(res))
              old= 1;
            else
            {
              MYSQL_ROW row= mysql_fetch_row(res);
              old= !strncmp(row[1], "ON", 2);
            }
            mysql_free_result(res);
          }
        }
        if (old)
          make_scrambled_password_323(crypted_pw, pw);
        else
          make_scrambled_password(crypted_pw, pw);
      }
      else
	crypted_pw[0]=0;			/* No password */
      sprintf(buff,"set password='%s',sql_log_off=0",crypted_pw);

      if (mysql_query(mysql,"set sql_log_off=1"))
      {
	my_printf_error(0, "Can't turn off logging; error: '%s'",
			MYF(ME_BELL),mysql_error(mysql));
	return -1;
      }
      if (mysql_query(mysql,buff))
      {
	if (mysql_errno(mysql)!=1290)
	{
	  my_printf_error(0,"unable to change password; error: '%s'",
			  MYF(ME_BELL),mysql_error(mysql));
	  return -1;
	}
	else
	{
	  /*
	    We don't try to execute 'update mysql.user set..'
	    because we can't perfectly find out the host
	   */
	  my_printf_error(0,"\n"
			  "You cannot use 'password' command as mysqld runs\n"
			  " with grant tables disabled (was started with"
			  " --skip-grant-tables).\n"
			  "Use: \"mysqladmin flush-privileges password '*'\""
			  " instead", MYF(ME_BELL));
	  return -1;
	}
      }
      argc--; argv++;
      break;
    }

    case ADMIN_START_SLAVE:
      if (mysql_query(mysql, "START SLAVE"))
      {
	my_printf_error(0, "Error starting slave: %s", MYF(ME_BELL),
			mysql_error(mysql));
	return -1;
      }
      else
	puts("Slave started");
      break;
    case ADMIN_STOP_SLAVE:
      if (mysql_query(mysql, "STOP SLAVE"))
      {
	  my_printf_error(0, "Error stopping slave: %s", MYF(ME_BELL),
			  mysql_error(mysql));
	  return -1;
      }
      else
	puts("Slave stopped");
      break;

    case ADMIN_PING:
      mysql->reconnect=0;	/* We want to know of reconnects */
      if (!mysql_ping(mysql))
      {
	if (option_silent < 2)
	  puts("mysqld is alive");
      }
      else
      {
	if (mysql_errno(mysql) == CR_SERVER_GONE_ERROR)
	{
	  mysql->reconnect=1;
	  if (!mysql_ping(mysql))
	    puts("connection was down, but mysqld is now alive");
	}
	else
	{
	  my_printf_error(0,"mysqld doesn't answer to ping, error: '%s'",
			  MYF(ME_BELL),mysql_error(mysql));
	  return -1;
	}
      }
      mysql->reconnect=1;	/* Automatic reconnect is default */
      break;
#ifdef LATER_HAVE_NDBCLUSTER_DB
    case ADMIN_NDB_MGM:
    {
      if (argc < 2)
      {
	my_printf_error(0,"Too few arguments to ndb-mgm",MYF(ME_BELL));
	return 1;
      }
      {
        Ndb_mgmclient_handle cmd=
	  ndb_mgmclient_handle_create(opt_ndb_connectstring);
        ndb_mgmclient_execute(cmd, --argc, ++argv);
	ndb_mgmclient_handle_destroy(cmd);
      }
      argc= 0;
    }
    break;
#endif
    default:
      my_printf_error(0,"Unknown command: '%-.60s'",MYF(ME_BELL),argv[0]);
      return 1;
    }
  }
  return 0;
}

#include <help_start.h>

static void print_version(void)
{
  printf("%s  Ver %s Distrib %s, for %s on %s\n",my_progname,ADMIN_VERSION,
	 MYSQL_SERVER_VERSION,SYSTEM_TYPE,MACHINE_TYPE);
  NETWARE_SET_SCREEN_MODE(1);
}


static void usage(void)
{
  print_version();
  puts("Copyright (C) 2000 MySQL AB & MySQL Finland AB & TCX DataKonsult AB");
  puts("This software comes with ABSOLUTELY NO WARRANTY. This is free software,\nand you are welcome to modify and redistribute it under the GPL license\n");
  puts("Administration program for the mysqld daemon.");
  printf("Usage: %s [OPTIONS] command command....\n", my_progname);
  my_print_help(my_long_options);
  my_print_variables(my_long_options);
  print_defaults("my",load_default_groups);
  puts("\nWhere command is a one or more of: (Commands may be shortened)\n\
  create databasename	Create a new database\n\
  debug			Instruct server to write debug information to log\n\
  drop databasename	Delete a database and all its tables\n\
  extended-status       Gives an extended status message from the server\n\
  flush-hosts           Flush all cached hosts\n\
  flush-logs            Flush all logs\n\
  flush-status		Clear status variables\n\
  flush-tables          Flush all tables\n\
  flush-threads         Flush the thread cache\n\
  flush-privileges      Reload grant tables (same as reload)\n\
  kill id,id,...	Kill mysql threads");
#if MYSQL_VERSION_ID >= 32200
  puts("\
  password new-password Change old password to new-password, MySQL 4.1 hashing.\n\
  old-password new-password Change old password to new-password in old format.\n");
#endif
  puts("\
  ping			Check if mysqld is alive\n\
  processlist		Show list of active threads in server\n\
  reload		Reload grant tables\n\
  refresh		Flush all tables and close and open logfiles\n\
  shutdown		Take server down\n\
  status		Gives a short status message from the server\n\
  start-slave		Start slave\n\
  stop-slave		Stop slave\n\
  variables             Prints variables available\n\
  version		Get version info from server");
}

#include <help_end.h>

static int drop_db(MYSQL *mysql, const char *db)
{
  char name_buff[FN_REFLEN+20], buf[10];
  if (!option_force)
  {
    puts("Dropping the database is potentially a very bad thing to do.");
    puts("Any data stored in the database will be destroyed.\n");
    printf("Do you really want to drop the '%s' database [y/N] ",db);
    fflush(stdout);
    VOID(fgets(buf,sizeof(buf)-1,stdin));
    if ((*buf != 'y') && (*buf != 'Y'))
    {
      puts("\nOK, aborting database drop!");
      return -1;
    }
  }
  sprintf(name_buff,"drop database `%.*s`",FN_REFLEN,db);
  if (mysql_query(mysql,name_buff))
  {
    my_printf_error(0,"DROP DATABASE %s failed;\nerror: '%s'",MYF(ME_BELL),
		    db,mysql_error(mysql));
    return 1;
  }
  printf("Database \"%s\" dropped\n",db);
  return 0;
}


static void nice_time(ulong sec,char *buff)
{
  ulong tmp;

  if (sec >= 3600L*24)
  {
    tmp=sec/(3600L*24);
    sec-=3600L*24*tmp;
    buff=int10_to_str(tmp, buff, 10);
    buff=strmov(buff,tmp > 1 ? " days " : " day ");
  }
  if (sec >= 3600L)
  {
    tmp=sec/3600L;
    sec-=3600L*tmp;
    buff=int10_to_str(tmp, buff, 10);
    buff=strmov(buff,tmp > 1 ? " hours " : " hour ");
  }
  if (sec >= 60)
  {
    tmp=sec/60;
    sec-=60*tmp;
    buff=int10_to_str(tmp, buff, 10);
    buff=strmov(buff," min ");
  }
  strmov(int10_to_str(sec, buff, 10)," sec");
}


static void print_header(MYSQL_RES *result)
{
  MYSQL_FIELD *field;

  print_top(result);
  mysql_field_seek(result,0);
  putchar('|');
  while ((field = mysql_fetch_field(result)))
  {
    printf(" %-*s|",(int) field->max_length+1,field->name);
  }
  putchar('\n');
  print_top(result);
}


static void print_top(MYSQL_RES *result)
{
  uint i,length;
  MYSQL_FIELD *field;

  putchar('+');
  mysql_field_seek(result,0);
  while((field = mysql_fetch_field(result)))
  {
    if ((length=(uint) strlen(field->name)) > field->max_length)
      field->max_length=length;
    else
      length=field->max_length;
    for (i=length+2 ; i--> 0 ; )
      putchar('-');
    putchar('+');
  }
  putchar('\n');
}


/* 3.rd argument, uint row, is not in use. Don't remove! */
static void print_row(MYSQL_RES *result, MYSQL_ROW cur,
		      uint row __attribute__((unused)))
{
  uint i,length;
  MYSQL_FIELD *field;

  putchar('|');
  mysql_field_seek(result,0);
  for (i=0 ; i < mysql_num_fields(result); i++)
  {
    field = mysql_fetch_field(result);
    length=field->max_length;
    printf(" %-*s|",length+1,cur[i] ? (char*) cur[i] : "");
  }
  putchar('\n');
}


static void print_relative_row(MYSQL_RES *result, MYSQL_ROW cur, uint row)
{
  ulonglong tmp;
  char buff[22];
  MYSQL_FIELD *field;

  mysql_field_seek(result, 0);
  field = mysql_fetch_field(result);
  printf("| %-*s|", (int) field->max_length + 1, cur[0]);

  field = mysql_fetch_field(result);
  tmp = cur[1] ? strtoull(cur[1], NULL, 10) : (ulonglong) 0;
  printf(" %-*s|\n", (int) field->max_length + 1,
	 llstr((tmp - last_values[row]), buff));
  last_values[row] = tmp;
}


static void print_relative_row_vert(MYSQL_RES *result __attribute__((unused)),
				    MYSQL_ROW cur,
				    uint row __attribute__((unused)))
{
  uint length;
  ulonglong tmp;
  char buff[22];

  if (!row)
    putchar('|');

  tmp = cur[1] ? strtoull(cur[1], NULL, 10) : (ulonglong) 0;
  printf(" %-*s|", ex_val_max_len[row] + 1,
	 llstr((tmp - last_values[row]), buff));

  /* Find the minimum row length needed to output the relative value */
  if ((length=(uint) strlen(buff) > ex_val_max_len[row]) && ex_status_printed)
    ex_val_max_len[row] = length;
  last_values[row] = tmp;
}


static void store_values(MYSQL_RES *result)
{
  uint i;
  MYSQL_ROW row;
  MYSQL_FIELD *field;

  field = mysql_fetch_field(result);
  max_var_length = field->max_length;
  field = mysql_fetch_field(result);
  max_val_length = field->max_length;

  for (i = 0; (row = mysql_fetch_row(result)); i++)
  {
    strmov(ex_var_names[i], row[0]);
    last_values[i]=strtoull(row[1],NULL,10);
    ex_val_max_len[i]=2;		/* Default print width for values */
  }
  ex_var_count = i;
  return;
}


static void print_relative_header()
{
  uint i;

  putchar('|');
  for (i = 0; i < ex_var_count; i++)
    printf(" %-*s|", ex_val_max_len[i] + 1, truncated_var_names[i]);
  putchar('\n');
}


static void print_relative_line()
{
  uint i;

  putchar('+');
  for (i = 0; i < ex_var_count; i++)
  {
    uint j;
    for (j = 0; j < ex_val_max_len[i] + 2; j++)
      putchar('-');
    putchar('+');
  }
  putchar('\n');
}


static void truncate_names()
{
  uint i;
  char *ptr,top_line[MAX_TRUNC_LENGTH+4+NAME_LEN+22+1],buff[22];

  ptr=top_line;
  *ptr++='+';
  ptr=strfill(ptr,max_var_length+2,'-');
  *ptr++='+';
  ptr=strfill(ptr,MAX_TRUNC_LENGTH+2,'-');
  *ptr++='+';
  ptr=strfill(ptr,max_val_length+2,'-');
  *ptr++='+';
  *ptr=0;
  puts(top_line);

  for (i = 0 ; i < ex_var_count; i++)
  {
    uint sfx=1,j;
    printf("| %-*s|", max_var_length + 1, ex_var_names[i]);
    ptr = ex_var_names[i];
    /* Make sure no two same truncated names will become */
    for (j = 0; j < i; j++)
      if (*truncated_var_names[j] == *ptr)
	sfx++;

    truncated_var_names[i][0]= *ptr;		/* Copy first var char */
    int10_to_str(sfx, truncated_var_names[i]+1,10);
    printf(" %-*s|", MAX_TRUNC_LENGTH + 1, truncated_var_names[i]);
    printf(" %-*s|\n", max_val_length + 1, llstr(last_values[i],buff));
  }
  puts(top_line);
  return;
}


static my_bool get_pidfile(MYSQL *mysql, char *pidfile)
{
  MYSQL_RES* result;

  if (mysql_query(mysql, "SHOW VARIABLES LIKE 'pid_file'"))
  {
    my_printf_error(0,"query failed; error: '%s'",MYF(ME_BELL),
		    mysql_error(mysql));
  }
  result = mysql_store_result(mysql);
  if (result)
  {
    MYSQL_ROW row=mysql_fetch_row(result);
    if (row)
      strmov(pidfile, row[1]);
    mysql_free_result(result);
    return row == 0;				/* Error if row = 0 */
  }
  return 1;					/* Error */
}

/*
  Return 1 if pid file didn't disappear or change
*/

static my_bool wait_pidfile(char *pidfile, time_t last_modified,
			    struct stat *pidfile_status)
{
  char buff[FN_REFLEN];
  int error= 1;
  uint count= 0;
  DBUG_ENTER("wait_pidfile");

  system_filename(buff, pidfile);
  do
  {
    int fd;
    if ((fd= my_open(buff, O_RDONLY, MYF(0))) < 0)
    {
      error= 0;
      break;
    }
    (void) my_close(fd,MYF(0));
    if (last_modified && !stat(pidfile, pidfile_status))
    {
      if (last_modified != pidfile_status->st_mtime)
      {
	/* File changed;  Let's assume that mysqld did restart */
	if (opt_verbose)
	  printf("pid file '%s' changed while waiting for it to disappear!\nmysqld did probably restart\n",
		 buff);
	error= 0;
	break;
      }
    }
    if (count++ == opt_shutdown_timeout)
      break;
    sleep(1);
  } while (!interrupted);

  if (error)
  {
    DBUG_PRINT("warning",("Pid file didn't disappear"));
    fprintf(stderr,
	    "Warning;  Aborted waiting on pid file: '%s' after %d seconds\n",
	    buff, count-1);
  }
  DBUG_RETURN(error);
}

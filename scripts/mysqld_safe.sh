#!/bin/sh
# Copyright Abandoned 1996 TCX DataKonsult AB & Monty Program KB & Detron HB
# This file is public domain and comes with NO WARRANTY of any kind
#
# scripts to start the MySQL daemon and restart it if it dies unexpectedly
#
# This should be executed in the MySQL base directory if you are using a
# binary installation that has other paths than you are using.
#
# mysql.server works by first doing a cd to the base directory and from there
# executing mysqld_safe

KILL_MYSQLD=1;
MYSQLD=

trap '' 1 2 3 15			# we shouldn't let anyone kill us

umask 007

defaults=
case "$1" in
    --no-defaults|--defaults-file=*|--defaults-extra-file=*)
      defaults="$1"; shift
      ;;
esac

usage () {
        cat <<EOF
Usage: $0 [OPTIONS]
  --no-defaults              Don't read the system defaults file
  --defaults-file=FILE       Use the specified defaults file
  --defaults-extra-file=FILE Also use defaults from the specified file
  --ledir=DIRECTORY          Look for mysqld in the specified directory
  --log-error=FILE           Log errors to the specified log file
  --open-files-limit=LIMIT   Limit the number of open files
  --core-file-size=LIMIT     Limit core files to the specified size
  --timezone=TZ              Set the system timezone
  --mysqld=FILE              Use the specified file as mysqld
  --mysqld-version=VERSION   Use "mysqld-VERSION" as mysqld
  --nice=NICE                Set the scheduling priority of mysqld
  --skip-kill-mysqld         Don't try to kill stray mysqld processes

All other options are passed to the mysqld program.

EOF
        exit 1
}


parse_arguments() {
  # We only need to pass arguments through to the server if we don't
  # handle them here.  So, we collect unrecognized options (passed on
  # the command line) into the args variable.
  pick_args=
  if test "$1" = PICK-ARGS-FROM-ARGV
  then
    pick_args=1
    shift
  fi

  for arg do
    case "$arg" in
      --skip-kill-mysqld*)
        KILL_MYSQLD=0;
        ;;
      # these get passed explicitly to mysqld
      --basedir=*) MY_BASEDIR_VERSION=`echo "$arg" | sed -e "s;--basedir=;;"` ;;
      --datadir=*) DATADIR=`echo "$arg" | sed -e "s;--datadir=;;"` ;;
      --pid-file=*) pid_file=`echo "$arg" | sed -e "s;--pid-file=;;"` ;;
      --user=*) user=`echo "$arg" | sed -e "s;--[^=]*=;;"` ; SET_USER=1 ;;

      # these two might have been set in a [mysqld_safe] section of my.cnf
      # they are added to mysqld command line to override settings from my.cnf
      --socket=*)  mysql_unix_port=`echo "$arg" | sed -e "s;--socket=;;"` ;;
      --port=*)    mysql_tcp_port=`echo "$arg" | sed -e "s;--port=;;"` ;;

      # mysqld_safe-specific options - must be set in my.cnf ([mysqld_safe])!
      --ledir=*)   ledir=`echo "$arg" | sed -e "s;--ledir=;;"` ;;
      --log-error=*) err_log=`echo "$arg" | sed -e "s;--log-error=;;"` ;;
      --open-files-limit=*) open_files=`echo "$arg" | sed -e "s;--open-files-limit=;;"` ;;
      --core-file-size=*) core_file_size=`echo "$arg" | sed -e "s;--core-file-size=;;"` ;;
      --timezone=*) TZ=`echo "$arg" | sed -e "s;--timezone=;;"` ; export TZ; ;;
      --mysqld=*)   MYSQLD=`echo "$arg" | sed -e "s;--mysqld=;;"` ;;
      --mysqld-version=*)
	tmp=`echo "$arg" | sed -e "s;--mysqld-version=;;"`
	if test -n "$tmp"
	then
	  MYSQLD="mysqld-$tmp"
	else
	  MYSQLD="mysqld"
	fi
	;;
      --nice=*) niceness=`echo "$arg" | sed -e "s;--nice=;;"` ;;
      --help)
        usage
        ;;
      *)
        if test -n "$pick_args"
        then
          # This sed command makes sure that any special chars are quoted,
          # so the arg gets passed exactly to the server.
          args="$args "`echo "$arg" | sed -e 's,\([^a-zA-Z0-9_.-]\),\\\\\1,g'`
        fi
        ;;
    esac
  done
}


#
# First, try to find BASEDIR and ledir (where mysqld is)
# 

MY_PWD=`pwd`
# Check for the directories we would expect from a binary release install
if test -f ./share/mysql/english/errmsg.sys -a -x ./bin/mysqld
then
  MY_BASEDIR_VERSION=$MY_PWD		# Where bin, share and data are
  ledir=$MY_BASEDIR_VERSION/bin		# Where mysqld is
# Check for the directories we would expect from a source install
elif test -f ./share/mysql/english/errmsg.sys -a \
 -x ./libexec/mysqld
then
  MY_BASEDIR_VERSION=$MY_PWD		# Where libexec, share and var are
  ledir=$MY_BASEDIR_VERSION/libexec	# Where mysqld is
# Since we didn't find anything, used the compiled-in defaults
else
  MY_BASEDIR_VERSION=@prefix@
  ledir=@libexecdir@
fi

#
# Second, try to find the data directory
#

# Try where the binary installs put it
if test -d $MY_BASEDIR_VERSION/data/mysql
then
  DATADIR=$MY_BASEDIR_VERSION/data
  if test -z "$defaults" -a -r "$DATADIR/my.cnf"
  then
    defaults="--defaults-extra-file=$DATADIR/my.cnf"
  fi
# Next try where the source installs put it
elif test -d $MY_BASEDIR_VERSION/var/mysql
then
  DATADIR=$MY_BASEDIR_VERSION/var
# Or just give up and use our compiled-in default
else
  DATADIR=@localstatedir@
fi

if test -z "$MYSQL_HOME"
then 
  if test -r "$MY_BASEDIR_VERSION/my.cnf" && test -r "$DATADIR/my.cnf"
  then
    echo "WARNING: Found two instances of my.cnf -"
    echo "$MY_BASEDIR_VERSION/my.cnf and"
    echo "$DATADIR/my.cnf"
    echo "IGNORING $DATADIR/my.cnf"
    echo
    MYSQL_HOME=$MY_BASEDIR_VERSION
  elif test -r "$DATADIR/my.cnf"
  then
    echo "WARNING: Found $DATADIR/my.cnf"
    echo "Datadir is deprecated place for my.cnf, please move it to $MY_BASEDIR_VERSION"
    echo
    MYSQL_HOME=$DATADIR
  else
    MYSQL_HOME=$MY_BASEDIR_VERSION
  fi
fi
export MYSQL_HOME

user=@MYSQLD_USER@
niceness=0

# these rely on $DATADIR by default, so we'll set them later on
pid_file=
err_log=

# Get first arguments from the my.cnf file, groups [mysqld] and [mysqld_safe]
# and then merge with the command line arguments
if test -x ./bin/my_print_defaults
then
  print_defaults="./bin/my_print_defaults"
elif test -x @bindir@/my_print_defaults
then
  print_defaults="@bindir@/my_print_defaults"
elif test -x @bindir@/mysql_print_defaults
then
  print_defaults="@bindir@/mysql_print_defaults"
else
  print_defaults="my_print_defaults"
fi

args=
SET_USER=2
parse_arguments `$print_defaults $defaults --loose-verbose mysqld server`
if test $SET_USER -eq 2
then
  SET_USER=0
fi
parse_arguments `$print_defaults $defaults --loose-verbose mysqld_safe safe_mysqld`
parse_arguments PICK-ARGS-FROM-ARGV "$@"
safe_mysql_unix_port=${mysql_unix_port:-${MYSQL_UNIX_PORT:-@MYSQL_UNIX_ADDR@}}

# Make sure that directory for $safe_mysql_unix_port exists
mysql_unix_port_dir=`dirname $safe_mysql_unix_port`
if [ ! -d $mysql_unix_port_dir ]
then
  mkdir $mysql_unix_port_dir
  chown $user $mysql_unix_port_dir
fi

# Use the mysqld-max binary by default if the user doesn't specify a binary
if test -z "$MYSQLD"
then
  if test -x $ledir/mysqld-max
  then
    MYSQLD=mysqld-max
  else
    MYSQLD=mysqld
  fi
fi

if test ! -x $ledir/$MYSQLD
then
  echo "The file $ledir/$MYSQLD doesn't exist or is not executable"
  echo "Please do a cd to the mysql installation directory and restart"
  echo "this script from there as follows:"
  echo "./bin/mysqld_safe".
  echo "See http://dev.mysql.com/doc/mysql/en/mysqld_safe.html for more"
  echo "information"
  exit 1
fi

if test -z "$pid_file"
then
  pid_file=$DATADIR/`@HOSTNAME@`.pid
else
  case "$pid_file" in
    /* ) ;;
    * )  pid_file="$DATADIR/$pid_file" ;;
  esac
fi
test -z "$err_log"  && err_log=$DATADIR/`@HOSTNAME@`.err

if test -n "$mysql_unix_port"
then
  args="--socket=$mysql_unix_port $args"
fi
if test -n "$mysql_tcp_port"
then
  args="--port=$mysql_tcp_port $args"
fi

if test $niceness -eq 0
then
  NOHUP_NICENESS="nohup"
else
  NOHUP_NICENESS="nohup nice -$niceness"
fi

# Using nice with no args to get the niceness level is GNU-specific.
# This check could be extended for other operating systems (e.g.,
# BSD could use "nohup sh -c 'ps -o nice -p $$' | tail -1").
# But, it also seems that GNU nohup is the only one which messes
# with the priority, so this is okay.
if nohup nice > /dev/null 2>&1
then
    normal_niceness=`nice`
    nohup_niceness=`nohup nice`

    numeric_nice_values=1
    for val in $normal_niceness $nohup_niceness
    do
        case "$val" in
            -[0-9] | -[0-9][0-9] | -[0-9][0-9][0-9] | \
             [0-9] |  [0-9][0-9] |  [0-9][0-9][0-9] )
                ;;
            * )
                numeric_nice_values=0 ;;
        esac
    done

    if test $numeric_nice_values -eq 1
    then
        nice_value_diff=`expr $nohup_niceness - $normal_niceness`
        if test $? -eq 0 && test $nice_value_diff -gt 0 && \
            nice --$nice_value_diff echo testing > /dev/null 2>&1
        then
            # nohup increases the priority (bad), and we are permitted
            # to lower the priority with respect to the value the user
            # might have been given
            niceness=`expr $niceness - $nice_value_diff`
            NOHUP_NICENESS="nice -$niceness nohup"
        fi
    fi
else
    if nohup echo testing > /dev/null 2>&1
    then
        :
    else
        # nohup doesn't work on this system
        NOHUP_NICENESS=""
    fi
fi

USER_OPTION=""
if test -w / -o "$USER" = "root"
then
  if test "$user" != "root" -o $SET_USER = 1
  then
    USER_OPTION="--user=$user"
  fi
  # If we are root, change the err log to the right user.
  touch $err_log; chown $user $err_log
  if test -n "$open_files"
  then
    ulimit -n $open_files
    args="--open-files-limit=$open_files $args"
  fi
  if test -n "$core_file_size"
  then
    ulimit -c $core_file_size
  fi
fi

#
# If there exists an old pid file, check if the daemon is already running
# Note: The switches to 'ps' may depend on your operating system
if test -f $pid_file
then
  PID=`cat $pid_file`
  if @CHECK_PID@
  then
    if @FIND_PROC@
    then    # The pid contains a mysqld process
      echo "A mysqld process already exists"
      echo "A mysqld process already exists at " `date` >> $err_log
      exit 1
    fi
  fi
  rm -f $pid_file
  if test -f $pid_file
  then
    echo "Fatal error: Can't remove the pid file: $pid_file"
    echo "Fatal error: Can't remove the pid file: $pid_file at " `date` >> $err_log
    echo "Please remove it manually and start $0 again"
    echo "mysqld daemon not started"
    exit 1
  fi
fi

#
# Uncomment the following lines if you want all tables to be automatically
# checked and repaired during startup. You should add sensible key_buffer
# and sort_buffer values to my.cnf to improve check performance or require
# less disk space.
# Alternatively, you can start mysqld with the "myisam-recover" option. See
# the manual for details.
#
# echo "Checking tables in $DATADIR"
# $MY_BASEDIR_VERSION/bin/myisamchk --silent --force --fast --medium-check $DATADIR/*/*.MYI
# $MY_BASEDIR_VERSION/bin/isamchk --silent --force $DATADIR/*/*.ISM

echo "Starting $MYSQLD daemon with databases from $DATADIR"

# Does this work on all systems?
#if type ulimit | grep "shell builtin" > /dev/null
#then
#  ulimit -n 256 > /dev/null 2>&1		# Fix for BSD and FreeBSD systems
#fi

echo "`date +'%y%m%d %H:%M:%S  mysqld started'`" >> $err_log
while true
do
  rm -f $safe_mysql_unix_port $pid_file	# Some extra safety
  if test -z "$args"
  then
    $NOHUP_NICENESS $ledir/$MYSQLD $defaults --basedir=$MY_BASEDIR_VERSION --datadir=$DATADIR $USER_OPTION --pid-file=$pid_file @MYSQLD_DEFAULT_SWITCHES@ >> $err_log 2>&1
  else
    eval "$NOHUP_NICENESS $ledir/$MYSQLD $defaults --basedir=$MY_BASEDIR_VERSION --datadir=$DATADIR $USER_OPTION --pid-file=$pid_file @MYSQLD_DEFAULT_SWITCHES@ $args >> $err_log 2>&1"
  fi
  if test ! -f $pid_file		# This is removed if normal shutdown
  then
    echo "STOPPING server from pid file $pid_file"
    break
  fi

  if @TARGET_LINUX@ && test $KILL_MYSQLD -eq 1
  then
    # Test if one process was hanging.
    # This is only a fix for Linux (running as base 3 mysqld processes)
    # but should work for the rest of the servers.
    # The only thing is ps x => redhat 5 gives warnings when using ps -x.
    # kill -9 is used or the process won't react on the kill.
    numofproces=`ps xaww | grep -v "grep" | grep "$ledir/$MYSQLD\>" | grep -c "pid-file=$pid_file"`

    echo -e "\nNumber of processes running now: $numofproces" | tee -a $err_log
    I=1
    while test "$I" -le "$numofproces"
    do 
      PROC=`ps xaww | grep "$ledir/$MYSQLD\>" | grep -v "grep" | grep "pid-file=$pid_file" | sed -n '$p'` 

      for T in $PROC
      do
        break
      done
      #    echo "TEST $I - $T **"
      if kill -9 $T
      then
        echo "$MYSQLD process hanging, pid $T - killed" | tee -a $err_log
      else 
        break
      fi
      I=`expr $I + 1`
    done
  fi
  echo "`date +'%y%m%d %H:%M:%S'`  mysqld restarted" | tee -a $err_log
done

echo "`date +'%y%m%d %H:%M:%S'`  mysqld ended" | tee -a $err_log
echo "" | tee -a $err_log


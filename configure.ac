dnl --------------------------------
dnl Initialization macros.
dnl --------------------------------

AC_INIT([rdmamon],[1.3.2])
AM_INIT_AUTOMAKE

dnl -----------------------------------------------
dnl Checks for programs.
dnl -----------------------------------------------

AC_PROG_CC
AM_PROG_CC_C_O

AC_CANONICAL_HOST
case $host in
  i?86-*-linux-*) ARCH=IA32;;
  ia64-*-linux-*) ARCH=IA64;;
  x86_64-*-linux-*) ARCH=AMD64;;
  armv6*-*-linux-*) ARCH=ARMv6;;
  *) ARCH=UNSUPPORTED;;
esac
AC_MSG_RESULT( Architecture ... $ARCH)
AC_SUBST(ARCH)


dnl Location of HJ tools
AC_ARG_WITH(hj_location,
  [  --with-hj-location=<PATH>         Specify the location of the hj tools],
  [ HJ_LOCATION="$withval" ],
  [ HJ_LOCATION=/usr ]
)
AC_SUBST(HJ_LOCATION)

case $ARCH in
  IA64)  LIBS="-lm -L$HJ_LOCATION/lib64";;
  AMD64) LIBS="-lm -L$HJ_LOCATION/libAMD64";;
  ARMv6) LIBS="-lm -L$HJ_LOCATION/libARM";;
  *)     LIBS="-lm -L$HJ_LOCATION/lib";;
esac

dnl Check for HJ tools and add library to LIBS
AC_CHECK_LIB([hj], [CPUStat_Start], , AC_MSG_ERROR([libhj was not found]))
AC_CHECK_LIB([pthread], [pthread_create], , AC_MSG_ERROR([pthread was not found]))
AC_CHECK_LIB([ibverbs], [ibv_cmd_poll_cq], , AC_MSG_ERROR([RDMA verbs was not found]))
AC_CHECK_LIB([rdmacm], [rdma_listen], , AC_MSG_ERROR([RDMA verbs was not found]))

dnl -----------------------------------------------
dnl Let's see if the system really supports IPv4.
dnl -----------------------------------------------

AC_MSG_CHECKING(for IPv4 support)
AC_CACHE_VAL(ac_cv_ipv4,
	AC_RUN_IFELSE([AC_LANG_SOURCE([[
#	include <sys/types.h>
#	include <sys/socket.h>
#	include <netinet/in.h>
	/* Make sure the definitions for AF_INET and struct sockaddr_in
	 * are defined, and that we can actually create an IPv4 TCP socket.
	 */
	main()
	{
		int fd;
		struct sockaddr_in foo;
		fd = socket(AF_INET, SOCK_STREAM, 0);
		exit(fd >= 0 ? 0 : 1);
	}]])],[ac_cv_ipv4=yes],[ac_cv_ipv4=no],[ac_cv_ipv4=no]))
AC_MSG_RESULT($ac_cv_ipv4)
if test $ac_cv_ipv4 = yes ; then
	AC_DEFINE(IPV4)
	AC_DEFINE(IPv4)
fi

dnl -----------------------------------------------
dnl Let's see if the system really supports IPv6.
dnl -----------------------------------------------

AC_MSG_CHECKING(for IPv6 support)
AC_CACHE_VAL(ac_cv_ipv6,
	AC_RUN_IFELSE([AC_LANG_SOURCE([[
#	include <sys/types.h>
#	include <sys/socket.h>
#	include <netinet/in.h>
	/* Make sure the definitions for AF_INET6 and struct sockaddr_in6
	 * are defined, and that we can actually create an IPv6 TCP socket.
	 */
	main()
	{
		int fd;
		struct sockaddr_in6 foo;
		fd = socket(AF_INET6, SOCK_STREAM, 0);
		exit(fd >= 0 ? 0 : 1);
	}]])],[ac_cv_ipv6=yes],[ac_cv_ipv6=no],[ac_cv_ipv6=no]))
AC_MSG_RESULT($ac_cv_ipv6)
if test $ac_cv_ipv6 = yes ; then
	AC_DEFINE(IPV6)
	AC_DEFINE(IPv6)
fi
dnl -----------------------------------------------
dnl Let's see if the system really supports multicasting.
dnl -----------------------------------------------
AC_MSG_CHECKING(for multicast support)
AC_CACHE_VAL(ac_cv_multicast,
	AC_RUN_IFELSE([AC_LANG_SOURCE([[
#	include <sys/types.h>
#	include <sys/socket.h>
#	include <netinet/in.h>
	main()
	{
		int fd;
		unsigned char flag = 1;
		struct sockaddr_in foo;
		struct ip_mreq mreq;
		fd = socket(AF_INET, SOCK_DGRAM, 0);
		if (fd < 0) exit(1);
		if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP,
			       (void*)&flag, sizeof(flag)) < 0)
			exit(1);
		exit(0);
	}]])],[ac_cv_multicast=yes],[ac_cv_multicast=no],[ac_cv_multicast=no]))
AC_MSG_RESULT($ac_cv_multicast)
if test $ac_cv_multicast = yes ; then
	AC_DEFINE(MCAST)
fi

dnl -----------------------------------------------
dnl Substitutions in .in files
dnl Generates Makefiles.
dnl -----------------------------------------------

AC_CONFIG_FILES([Makefile
rdmamon.spec
version.h
])
AC_OUTPUT

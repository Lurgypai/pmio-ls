
dnl PAC_SET_HEADER_LIB_PATH(with_option)
dnl This macro looks for the --with-xxx=, --with-xxx-include and --with-xxx-lib=
dnl options and sets the library and include paths.
AC_DEFUN([PAC_SET_HEADER_LIB_PATH],[
    AC_ARG_WITH($1, 
    		AC_HELP_STRING([--with-$1=path],
			       [specify path where $1 include directory and lib directory can be found]),
        if test "${with_$1}" != "yes" -a "${with_$1}" != "no" ; then
            # is adding lib64 by default really the right thing to do?  What if
            # we are on a 32-bit host that happens to have both lib dirs available?
            LDFLAGS="$LDFLAGS -L${with_$1}/lib64 -L${with_$1}/lib"
            CPPFLAGS="$CPPFLAGS -I${with_$1}/include"
	    WRAPPER_CFLAGS="$WRAPPER_CFLAGS -I${with_$1}/include"
        fi,
    )
    AC_ARG_WITH($1-include, 
    		AC_HELP_STRING([--with-$1-include=path],
			       [specify path where $1 include directory can be found]),
        if test "${with_$1_include}" != "yes" -a "${with_$1_include}" != "no" ; then
            CPPFLAGS="$CPPFLAGS -I${with_$1_include}"
            WRAPPER_CFLAGS="$WRAPPER_CFLAGS -I${with_$1_include}"
        fi,
    )
    AC_ARG_WITH($1-lib, 
    		AC_HELP_STRING([--with-$1-lib=path],
			       [specify path where $1 lib directory can be found]),
        if test "${with_$1_lib}" != "yes" -a "${with_$1_lib}" != "no" ; then
            LDFLAGS="$LDFLAGS -L${with_$1_lib}"
        fi,
    )
])


dnl PAC_CHECK_HEADER_LIB(header.h, libname, function, action-if-yes, action-if-no)
dnl This macro checks for a header and lib.  It is assumed that the
dnl user can specify a path to the includes and libs using --with-xxx=.
dnl The xxx is specified in the "with_option" parameter.
AC_DEFUN([PAC_CHECK_HEADER_LIB],[
    failure=no
    AC_CHECK_HEADER([$1],,failure=yes)
    AC_CHECK_LIB($2,$3,,failure=yes)
    if test "$failure" = "no" ; then
       $4
    else
       $5
    fi
])

dnl PAC_CHECK_HEADER_LIB_FATAL(with_option, header.h, libname, function)
dnl Similar to PAC_CHECK_HEADER_LIB, but errors out on failure
AC_DEFUN([PAC_CHECK_HEADER_LIB_FATAL],[
	PAC_CHECK_HEADER_LIB($2,$3,$4,success=yes,success=no)
	if test "$success" = "no" ; then
	   AC_MSG_ERROR(['$2 or lib$3 library not found. Did you specify --with-$1= or --with-$1-include= or --with-$1-lib=?'])
	fi
])

dnl PAC_CHECK_PREFIX(with_option,prefixvar)
AC_DEFUN([PAC_CHECK_PREFIX],[
	AC_ARG_WITH([$1-prefix],
            [AS_HELP_STRING([[--with-$1-prefix[=DIR]]], [use the $1
                            library installed in DIR, rather than the
                            one included in the distribution.  Pass
                            "embedded" to force usage of the included
                            $1 source.])],
            [if test "$withval" = "system" ; then
                 :
             elif test "$withval" = "embedded" ; then
                 :
             else
                 PAC_APPEND_FLAG([-I${with_$1_prefix}/include],[CPPFLAGS])
                 if test -d "${with_$1_prefix}/lib64" ; then
                     PAC_APPEND_FLAG([-L${with_$1_prefix}/lib64],[LDFLAGS])
                 fi
                 PAC_APPEND_FLAG([-L${with_$1_prefix}/lib],[LDFLAGS])
             fi
             ],
            [with_$1_prefix="embedded"])
	]
)

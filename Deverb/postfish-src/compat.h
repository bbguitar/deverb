/*
 *
 *  postfish
 *    
 *      Copyright (C) 2018 Xiph.Org
 *
 *  Postfish is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *   
 *  Postfish is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *   
 *  You should have received a copy of the GNU General Public License
 *  along with Postfish; see the file COPYING.  If not, write to the
 *  Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * 
 */

/* Work around M_PIl only being available in libc */
#ifndef M_PIl
# define M_PIl      3.141592653589793238462643383279502884L
#endif

/* Work around M_PI not being defined without _USE_MATH_DEFINES on MSVC */
#ifndef M_PI
# define M_PI       3.141592653589793238462643383279502884
#endif

/* Work-around lack of PATH_MAX */
#ifndef PATH_MAX
# define PATH_MAX 1024
#endif

/* Windows: map POSIX pthreads to Win32 CRITICAL_SECTION */
#ifdef _WIN32
# include <windows.h>
# include <malloc.h>
# define alloca _alloca
  typedef CRITICAL_SECTION pthread_mutex_t;
  typedef int pthread_mutexattr_t;
# define PTHREAD_MUTEX_RECURSIVE 0
  static inline int pthread_mutexattr_init(pthread_mutexattr_t *a){ (void)a; return 0; }
  static inline int pthread_mutexattr_settype(pthread_mutexattr_t *a, int t){ (void)a; (void)t; return 0; }
  static inline int pthread_mutex_init(pthread_mutex_t *m, pthread_mutexattr_t *a){
    (void)a; InitializeCriticalSection(m); return 0;
  }
  static inline int pthread_mutex_lock(pthread_mutex_t *m){
    EnterCriticalSection(m); return 0;
  }
  static inline int pthread_mutex_unlock(pthread_mutex_t *m){
    LeaveCriticalSection(m); return 0;
  }
#endif

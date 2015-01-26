#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <mono/metadata/profiler.h>
#include <mono/metadata/debug-helpers.h>

/*
 * Profiling backend for privateeye in mono. Uses the mono profiler api and
 * writes to a named pipe that gets processed by the privateeye backend
 * in the host process.
 * TODO add more comments and stuff
 *  */


#define LOCK_PROFILER() do { pthread_mutex_lock (&(profiler->mutex));} while (0)
#define UNLOCK_PROFILER() do { pthread_mutex_unlock (&(profiler->mutex));} while (0)

#define ENABLE_PROFILER() profiler->enabled = TRUE;
#define DISABLE_PROFILER() profiler->enabled = FALSE;

#define CHECK_PROFILER_ENABLED() do {\
    if (! profiler->enabled)\
        return;\
} while (0)

#define DELETE_PROFILER_MUTEX() pthread_mutex_destroy (&(profiler->mutex))

static char *START_OUTPUT_METHOD = "7A91E123-0BF7-4A17-AB35-D3879E65D032";
static char *END_OUTPUT_METHOD = "8BB1D222-103F-49FD-A93A-588E5BFE36B8";
static char *FORCE_FLUSH_PROFILER_METHOD = "33E04260-B266-42F6-8FC7-44FD7D3BF881";

static char *FIFO_PREXIX = "PRIVATE_EYE";

/* Time counting */
#define MONO_PROFILER_GET_CURRENT_TIME(t) {\
        struct timeval current_time;\
        gettimeofday (&current_time, NULL);\
        (t) = (((guint64)current_time.tv_sec) * 1000000) + current_time.tv_usec;\
} while (0)

static gboolean use_fast_timer = FALSE;

#if (defined(__i386__) || defined(__x86_64__)) && ! defined(HOST_WIN32)

#if defined(__i386__)
static const guchar cpuid_impl [] = {
        0x55,                       /* push   %ebp */
        0x89, 0xe5,                 /* mov    %esp,%ebp */
        0x53,                       /* push   %ebx */
        0x8b, 0x45, 0x08,           /* mov    0x8(%ebp),%eax */
        0x0f, 0xa2,                 /* cpuid   */
        0x50,                       /* push   %eax */
        0x8b, 0x45, 0x10,           /* mov    0x10(%ebp),%eax */
        0x89, 0x18,                 /* mov    %ebx,(%eax) */
        0x8b, 0x45, 0x14,           /* mov    0x14(%ebp),%eax */
        0x89, 0x08,                 /* mov    %ecx,(%eax) */
        0x8b, 0x45, 0x18,           /* mov    0x18(%ebp),%eax */
        0x89, 0x10,                 /* mov    %edx,(%eax) */
        0x58,                       /* pop    %eax */
        0x8b, 0x55, 0x0c,           /* mov    0xc(%ebp),%edx */
        0x89, 0x02,                 /* mov    %eax,(%edx) */
        0x5b,                       /* pop    %ebx */
        0xc9,                       /* leave   */
        0xc3,                       /* ret     */
};

typedef void (*CpuidFunc) (int id, int* p_eax, int* p_ebx, int* p_ecx, int* p_edx);

static int 
cpuid (int id, int* p_eax, int* p_ebx, int* p_ecx, int* p_edx) {
    int have_cpuid = 0;
#ifndef _MSC_VER
        __asm__  __volatile__ (
            "pushfl\n"
            "popl %%eax\n"
            "movl %%eax, %%edx\n"
            "xorl $0x200000, %%eax\n"
            "pushl %%eax\n"
            "popfl\n"
            "pushfl\n"
            "popl %%eax\n"
            "xorl %%edx, %%eax\n"
            "andl $0x200000, %%eax\n"
            "movl %%eax, %0"
            : "=r" (have_cpuid)
            :
            : "%eax", "%edx"
      );
#else
      __asm {
            pushfd
            pop eax
            mov edx, eax
            xor eax, 0x200000
            push eax
            popfd
            pushfd
            pop eax
            xor eax, edx
            and eax, 0x200000
            mov have_cpuid, eax
     }
#endif
     if (have_cpuid) {
         CpuidFunc func = (CpuidFunc) cpuid_impl;
         func (id, p_eax, p_ebx, p_ecx, p_edx);
         /*
          *       * We use this approach because of issues with gcc and pic code, see:
          *       * http://gcc.gnu.org/cgi-bin/gnatsweb.pl?cmd=view%20audit-trail&database=gcc&pr=7329
          *       __asm__ __volatile__ ("cpuid"
          *       : "=a" (*p_eax), "=b" (*p_ebx), "=c" (*p_ecx), "=d" (*p_edx)
          *       : "a" (id));
          **/
          return 1;
      }
   return 0;
}

static void detect_fast_timer (void) {
    int p_eax, p_ebx, p_ecx, p_edx;
            
    if (cpuid (0x1, &p_eax, &p_ebx, &p_ecx, &p_edx)) {
        if (p_edx & 0x10) {
            use_fast_timer = TRUE;
        } else {
            use_fast_timer = FALSE;
        }
    } else {
        use_fast_timer = FALSE;
    }
}
#endif

#if defined(__x86_64__)
static void detect_fast_timer (void) {
    guint32 op = 0x1;
    guint32 eax,ebx,ecx,edx;
    __asm__ __volatile__ ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(op));
    if (edx & 0x10) {
        use_fast_timer = TRUE;
    } else {
        use_fast_timer = FALSE;
    }
}
#endif

static __inline__ guint64 rdtsc(void) {
        guint32 hi, lo;
        __asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
        return ((guint64) lo) | (((guint64) hi) << 32);
}
#define MONO_PROFILER_GET_CURRENT_COUNTER(c) {\
    if (use_fast_timer) {\
        (c) = rdtsc ();\
    } else {\
        MONO_PROFILER_GET_CURRENT_TIME ((c));\
    }\
} while (0)
#else
static void detect_fast_timer (void) {
        use_fast_timer = FALSE;
}
#define MONO_PROFILER_GET_CURRENT_COUNTER(c) MONO_PROFILER_GET_CURRENT_TIME ((c))
#endif

typedef struct _MethodIdMappingElement {
    char *name;
    guint32 id;
    MonoMethod *method;
} MethodIdMappingElement;

typedef struct _ClassIdMappingElement {
    char *name;
    guint32 id;
    MonoClass *klass;
} ClassIdMappingElement;

/* MonoProfiler Opaque struct */
struct _MonoProfiler {
    pthread_mutex_t mutex;
    int ncalls;
    int allocations;
    gboolean enabled;
    GHashTable *cached_methods;
    GHashTable *cached_types;
    FILE *fd;
};

static char * get_fifo_name() {
    return "";    
}

static void 
open_named_pipe_for_output (MonoProfiler *profiler, char *pipe_name) {
    FILE *pipe;

    mkfifo (pipe_name, 0666);
    pipe = (FILE *) fopen (pipe_name, "w");
    if(!pipe) {
        printf ("Could not open pipe, exiting");
        exit (1);
    }
    profiler->fd = pipe;
}

static void
open_file_for_output (MonoProfiler *profiler, char *name) {
    FILE *fd;

    fd = fopen ("file.txt", "w+");
    if(!fd) {
        perror ("error opening file, exiting");
        exit (1);
    }
    profiler->fd = (FILE *) fd; 
}

static void 
close_fifo (MonoProfiler *profiler) {
    if(!fclose (profiler->fd)) {
        perror ("couldn't close pipe");
        exit(1);
    }
}

static void
flush_buffer (MonoProfiler *profiler) {
    assert (profiler != NULL);
    assert (profiler->fd > 0);
    fflush (profiler->fd);
}

static MethodIdMappingElement*
get_method_name_cached(MonoProfiler *profiler, MonoMethod *method) {
    return g_hash_table_lookup (profiler->cached_methods, (gconstpointer) method);
}

static ClassIdMappingElement*
class_id_mapping_element_get (MonoProfiler *profiler, MonoClass *klass) {
    return g_hash_table_lookup (profiler->cached_types, (gconstpointer) klass);
}

static MethodIdMappingElement*
method_id_mapping_element_new (MonoProfiler *profiler, MonoMethod *method) {
    MethodIdMappingElement *result = g_new (MethodIdMappingElement, 1);
    char *signature = mono_signature_get_desc (mono_method_signature (method), TRUE);
                
    result->name = g_strdup_printf ("%s (%s)", mono_method_get_name (method), signature);
    g_free (signature);
    result->method = method;
    g_hash_table_insert (profiler->cached_methods, method, result);
#if (DEBUG_MAPPING_EVENTS)
    printf ("Created new METHOD mapping element \"%s\" (%p)[%d]\n", result->name, method, result->id);
#endif
    return result;
}


static void
method_id_mapping_element_destroy (gpointer element) {
    MethodIdMappingElement *e = (MethodIdMappingElement*) element;
    if (e->name)
        g_free (e->name);
    g_free (element);
}

static void
class_id_mapping_element_destroy (gpointer element) {
    ClassIdMappingElement *e = (ClassIdMappingElement*) element;
    if (e->name)
         g_free (e->name);
    g_free (element);
}

static char *
get_method_name(MonoProfiler *profiler, MonoMethod *method) {
    //TODO FIX ME
    MonoClass *klass = mono_method_get_class (method);
    if(klass == NULL) return NULL;
    if(method == NULL) return NULL;
    MonoMethodSignature *sig = mono_method_signature (method);
    if(sig == NULL) { printf("WTF NULL"); exit(1); }
    char *signature = (char *) mono_signature_get_desc (sig, TRUE);
    if(signature == NULL) return NULL; 
    void *returntype = mono_signature_get_return_type(sig);
    if (!returntype) { printf("return type null\n");}
    char *tname = mono_type_get_name(returntype);
    if (tname == NULL) printf("tname is NULL");
    char *name = g_strdup_printf ("%s %s.%s:%s (%s)", 
                                 tname, 
                                 mono_class_get_namespace (klass), 
                                 mono_class_get_name (klass), 
                                 mono_method_get_name (method), 
                                 signature);
    g_free (signature);
    g_free (tname);
    return name; 
}

static MonoProfiler *
init_mono_profiler () {
    MonoProfiler *ret;

    ret = (MonoProfiler *) malloc(sizeof(MonoProfiler)); 
    ret->cached_methods = g_hash_table_new_full (g_direct_hash, NULL, NULL, method_id_mapping_element_destroy);
    ret->cached_types = g_hash_table_new_full (g_direct_hash, NULL, NULL, class_id_mapping_element_destroy);
    
    pthread_mutex_init (&(ret->mutex), NULL);
    return ret;
}


/**********************************************************************
 *
 * Profiler callbacks
 *
 **********************************************************************/

static void
pe_shutdown (MonoProfiler *profiler) {
    LOCK_PROFILER();
    mono_profiler_set_events (0);
    fclose (profiler->fd);
    CHECK_PROFILER_ENABLED();
    printf("number of calls is %d\n", profiler->ncalls);
    printf("number of allocations is %d\n", profiler->allocations);
    UNLOCK_PROFILER();
}

static void
pe_method_enter (MonoProfiler *profiler, MonoMethod *method) {
    guint64 counter = 0;
    CHECK_PROFILER_ENABLED();
    MONO_PROFILER_GET_CURRENT_COUNTER (counter);
    char *name = get_method_name(profiler, method);
    printf("enter %lu %s\n", counter, name);
    profiler->ncalls++;
    //g_free (name);
}

static void
pe_method_leave (MonoProfiler *profiler, MonoMethod *method) {
    guint64 counter;
    CHECK_PROFILER_ENABLED();
    MONO_PROFILER_GET_CURRENT_COUNTER (counter);
    char *name = get_method_name(profiler, method);
    printf("leave %lu %s\n", counter, name);
    //g_free (name);
}

static void
pe_object_allocated (MonoProfiler *profiler, MonoObject *obj, MonoClass *klass) {
    CHECK_PROFILER_ENABLED();
    guint size = mono_object_get_size (obj);
    printf("allocate: %s.%s %d\n", mono_class_get_namespace (klass), mono_class_get_name (klass), size);
    profiler->allocations++;
}

static void
pe_method_jit_result (MonoProfiler *profiler, MonoMethod *method, MonoJitInfo* jinfo, int result) {
    CHECK_PROFILER_ENABLED();
    if (result == MONO_PROFILE_OK) {
        char *name = get_method_name (profiler, method);
        gpointer code_start = mono_jit_info_get_code_start (jinfo);
        int code_size = mono_jit_info_get_code_size (jinfo);
        if(name == NULL) return;
        //printf ("JIT completed %s\n", name);
        g_free (name);
    }
}

static void
pe_throw_callback (MonoProfiler *profiler, MonoObject *object) {
    fprintf(profiler->fd, "exception thrown\n");
    CHECK_PROFILER_ENABLED();
}

static void 
pe_clause_callback (MonoProfiler *profiler, MonoMethod *method, int clause_type, int clause_num) {
    fprintf(profiler->fd, "clause call back\n");
    CHECK_PROFILER_ENABLED();
}

static void
pe_exc_method_leave (MonoProfiler *profiler, MonoMethod *method) {
    //printf("method leave due to exception.\n");
    guint64 counter;
    CHECK_PROFILER_ENABLED();
    MONO_PROFILER_GET_CURRENT_COUNTER (counter);
    char *name = get_method_name(profiler, method);
    fprintf(profiler->fd, "leave %lu %s\n", counter, name);
    //g_free (name);    
}

/* The main entry point of profiler (called back from mono, defined in profile.h)*/
void
mono_profiler_startup (const char *desc) {
    MonoProfiler *profiler;
    
    profiler = init_mono_profiler ();
    
    open_file_for_output (profiler, "foo.bin");
    
    mono_profiler_install (profiler, pe_shutdown);
    mono_profiler_install_enter_leave (pe_method_enter, pe_method_leave);
    mono_profiler_install_exception (pe_throw_callback, pe_exc_method_leave, pe_clause_callback);
    mono_profiler_install_allocation (pe_object_allocated);
    mono_profiler_install_jit_end (pe_method_jit_result);

    mono_profiler_set_events (MONO_PROFILE_ENTER_LEAVE | 
                              MONO_PROFILE_JIT_COMPILATION | 
                              MONO_PROFILE_ALLOCATIONS | 
                              MONO_PROFILE_EXCEPTIONS);
}


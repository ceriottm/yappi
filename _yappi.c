/*

 yappi
 Yet Another Python Profiler

 Sumer Cip 2013

*/

#include "config.h"

#if !defined(HAVE_LONG_LONG)
#error "Yappi requires long longs!"
#endif

#ifdef IS_PY3K
#include "bytesobject.h"
#endif
#include "frameobject.h"
#include "callstack.h"
#include "hashtab.h"
#include "debug.h"
#include "timing.h"
#include "freelist.h"
#include "mem.h"

#ifdef IS_PY3K
PyDoc_STRVAR(_yappi__doc__, "Yet Another Python Profiler");
#endif

// linked list for holding callee/caller info in the pit
// we need to record the timing data on the pairs (parent, child)
typedef struct {
    unsigned int index;
    unsigned long callcount;
    unsigned long nonrecursive_callcount;   // how many times the child function is called non-recursively?
    long long tsubtotal;                    // the time that the child function spent excluding its children (include recursive parent-child calls)
    long long ttotal;                       // the total time that the child function spent
    struct _pit_children_info *next;
} _pit_children_info;

typedef struct {
    // we currently use frame pointer for identifying the running coroutine on
    // a specific _pit.
    PyFrameObject *frame;

    // the first time this coroutine is seen on stack
    long long t0;

    struct _coro *next;
} _coro;

// module definitions
typedef struct {
    PyObject *name;
    PyObject *modname;
    unsigned long lineno;
    unsigned long callcount;

    // the number of actual calls when the function is recursive.
    unsigned long nonrecursive_callcount;

    // time function spent excluding its children (include recursive calls)
    long long tsubtotal;

    // the total time that a function spent
    long long ttotal;
    unsigned int builtin;

    // a number that uniquely identifies the _pit during the lifetime of a profile
    // session (for multiple start/stop pairs) We could use PyCodeObject for this
    // too, but then there are builtin calls, it is better to have a custom id
    // per-pit instead of dealing with the keys and their references.
    unsigned int index;

    long tag;

    // concurrent running coroutines on this _pit
    _coro *coroutines;

    _pit_children_info *children;

    _htab *tagged_pits;
} _pit; // profile_item

typedef struct {
    _cstack *cs;
    _htab *rec_levels;
    _htab *pits;
    long id;                                // internal tid given by user callback or yappi. Will be unique per profile session.
    long tid;                               // the real OS thread id.
    PyObject *name;
    long long t0;                           // profiling start CPU time
    unsigned long sched_cnt;                // how many times this thread is scheduled

    PyThreadState *ts_ptr;
} _ctx; // context

typedef struct
{
    PyObject *efn;
    _ctx *ctx;
} _ctxfuncenumarg;

typedef struct {
    int builtins;
    int multithreaded;
} _flag; // flags passed from yappi.start()

// globals
static PyObject *YappiProfileError;
static _htab *contexts;
static _flag flags;
static _freelist *flpit;
static _freelist *flctx;
static int yappinitialized;
static unsigned int ycurfuncindex; // used for providing unique index for functions
static long ycurthreadindex;
static int yapphavestats;   // start() called at least once or stats cleared?
static int yapprunning;
static int paused;
static time_t yappstarttime;
static long long yappstarttick;
static long long yappstoptick;
static _ctx *prev_ctx = NULL;
static _ctx *current_ctx = NULL;
static _ctx *initial_ctx = NULL; // used for holding the context that called start()
static PyObject *context_id_callback = NULL;
static PyObject *tag_callback = NULL;
static PyObject *context_name_callback = NULL;
static PyObject *test_timings; // used for testing
static int _profile_event_running = 0;

// defines
#define UNINITIALIZED_STRING_VAL "N/A"

#ifdef IS_PY3K // string formatting helper functions compatible with with both 2.x and 3.x
#define PyStr_AS_CSTRING(s) PyUnicode_AsUTF8(s)
#define PyStr_Check(s) PyUnicode_Check(s)
#define PyStr_FromString(s) PyUnicode_FromString(s)
#define PyStr_FromFormatV(fmt, vargs) PyUnicode_FromFormatV(fmt, vargs)
#else // < Py3x
#define PyStr_AS_CSTRING(s) PyString_AS_STRING(s)
#define PyStr_Check(s) PyString_Check(s)
#define PyStr_FromString(s) PyString_FromString(s)
#define PyStr_FromFormatV(fmt, vargs) PyString_FromFormatV(fmt, vargs)
#endif

// forwards
static _ctx * _profile_thread(PyThreadState *ts);
static int _pitenumdel(_hitem *item, void *arg);

// funcs
static void _DebugPrintObjects(unsigned int arg_count, ...)
{
    
    // PyObject *key, *value;
    // Py_ssize_t pos = 0;

    // while (PyDict_Next(dict, &pos, &key, &value)) {
    //     printf("%s=%s\n", PyStr_AS_CSTRING(key), PyStr_AS_CSTRING(value));
    // }

    unsigned int i;
    va_list vargs;
    va_start(vargs, arg_count);

    for (i=0; i<arg_count; i++) {
        PyObject_Print(va_arg(vargs, PyObject *), stdout, Py_PRINT_RAW);
    }
    printf("\n");
    va_end(vargs);
}

int IS_ASYNC(PyFrameObject *frame)
{
#if defined(IS_PY3K) && PY_MINOR_VERSION >= 4
    return frame->f_code->co_flags & CO_COROUTINE || 
        frame->f_code->co_flags & CO_ITERABLE_COROUTINE ||
        frame->f_code->co_flags & CO_ASYNC_GENERATOR;
#else
    return 0;
#endif
}

static PyObject *
PyStr_FromFormat(const char *fmt, ...)
{
    PyObject* ret;
    va_list vargs;

    va_start(vargs, fmt);
    ret = PyStr_FromFormatV(fmt, vargs);
    va_end(vargs);
    return ret;
}

// module functions

static void
_log_err(unsigned int code)
{
    yerr("Internal Error. [%u]", code);
}

static _pit *
_create_pit(int tagged)
{
    _pit *pit;

    pit = flget(flpit);
    if (!pit)
        return NULL;

    pit->tagged_pits = NULL;
    if (!tagged) { // do not create tagged_pit table for tagged pits
        pit->tagged_pits = htcreate(HT_TAGGED_PIT_SIZE);
        if (!pit->tagged_pits)
            return NULL;
    }
    
    pit->callcount = 0;
    pit->nonrecursive_callcount = 0;
    pit->ttotal = 0;
    pit->tsubtotal = 0;
    pit->name = NULL;
    pit->modname = NULL;
    pit->lineno = 0;
    pit->builtin = 0;
    pit->index = ycurfuncindex++;
    pit->children = NULL;
    pit->coroutines = NULL;
    pit->tag = 0;

    return pit;
}

static _ctx *
_create_ctx(void)
{
    _ctx *ctx;

    ctx = flget(flctx);
    if (!ctx)
        return NULL;
    ctx->cs = screate(100);
    if (!ctx->cs)
        return NULL;

    ctx->pits = htcreate(HT_PIT_SIZE);
    if (!ctx->pits)
        return NULL;

    ctx->sched_cnt = 0;
    ctx->id = 0;
    ctx->tid = 0;
    ctx->name = NULL;
    ctx->t0 = tickcount();
    ctx->rec_levels = htcreate(HT_RLEVEL_SIZE);
    if (!ctx->rec_levels)
        return NULL;
    return ctx;
}

static PyObject *
_current_context_name(void)
{
    PyObject *name;

    if (!context_name_callback) {
        return NULL;
    }

    name = PyObject_CallFunctionObjArgs(context_name_callback, NULL);
    if (!name) {
        PyErr_Print();
        goto err;
    }

    if (name == Py_None) {
        // Name not available yet - will try again on the next call
        goto later;
    }


    if (!PyStr_Check(name)) {
        yerr("context name callback returned non-string");
        goto err;
    }

    return name;

err:
    PyErr_Clear();
    Py_CLEAR(context_name_callback);  /* Don't use the callback again. */
    Py_XDECREF(name);
    return NULL;
later:
    Py_XDECREF(name);
    return NULL;
}

static long
_current_tag(void)
{
    PyObject *r;
    uintptr_t result;

    if (!tag_callback) {
        return 0;
    }

    r = PyObject_CallFunctionObjArgs(tag_callback, NULL);
    if (!r) {
        PyErr_Print();
        goto error;
    }

    result = (uintptr_t)PyLong_AsLong(r);
    Py_DECREF(r);
    if (PyErr_Occurred()) {
        yerr("tag_callback returned non-integer");
        goto error;
    }

    return result;
error:
    PyErr_Clear();
    Py_CLEAR(tag_callback); // don't use callback again
    return 0;
}

static _pit*
_get_tagged_pit(_pit *cp, uintptr_t curr_tag)
{
    _hitem *it;
    _pit *new_pit;

    // no tag?
    if (curr_tag == 0) {
        return NULL;
    }

    // make some defensive checks
    if (cp->tag != 0 || !cp->tagged_pits) {
        _log_err(13);
        return NULL;
    }

    it = hfind(cp->tagged_pits, curr_tag);
    if (!it) {
        new_pit = _create_pit(1);
        if (!new_pit)
            return NULL;
        if (!hadd(cp->tagged_pits, curr_tag, (uintptr_t)new_pit))
            return NULL;
        
        new_pit->name = cp->name;
        new_pit->modname = cp->modname;
        new_pit->lineno = cp->lineno;
        new_pit->builtin = cp->builtin;
        new_pit->index = cp->index;
        new_pit->tag = curr_tag;
        return new_pit;
    }
    return (_pit *)it->val;
}

static uintptr_t
_current_context_id(PyThreadState *ts)
{
    uintptr_t rc;
    PyObject *callback_rc;
    if (context_id_callback) {
        callback_rc = PyObject_CallFunctionObjArgs(context_id_callback, NULL);
        if (!callback_rc) {
            PyErr_Print();
            goto error;
        }
        rc = (uintptr_t)PyLong_AsLong(callback_rc);
        Py_DECREF(callback_rc);
        if (PyErr_Occurred()) {
            yerr("context id callback returned non-integer");
            goto error;
        }
        return rc;
    } else {
        // Use thread_id instead of ts pointer, because when we create/delete many threads, some
        // of them do not show up in the thread_stats, because ts pointers are recycled in the VM.
        // Also, OS tids are recycled, too. The only valid way is to give ctx's custom tids which
        // are hold in a per-thread structure. Again: we use an integer instead of directly mapping the ctx
        // pointer to some per-thread structure because other threading libraries do not necessarily
        // have direct ThreadState->Thread mapping. Greenlets, for example, will only have a single
        // thread. Therefore, we need to identify the "context" concept independent from ThreadState 
        // objects.

        // TODO: Any more optimization? This has increased the runtime factor from 7x to 11x.
        // and also we may have a memory leak below. We maybe can optimize the common case.
        PyObject *d = PyThreadState_GetDict();
        PyObject *ytid = PyDict_GetItemString(d, "_yappi_tid");
        if (!ytid) {
            ytid = PyLong_FromLong(ycurthreadindex++);
            PyDict_SetItemString(d, "_yappi_tid", ytid);
        }
        rc = PyLong_AsLong(ytid);
        return rc;
    }

error:
    PyErr_Clear();
    Py_CLEAR(context_id_callback); // don't use callback again
    return 0;
}

static _ctx *
_thread2ctx(PyThreadState *ts)
{
    _hitem *it;
    it = hfind(contexts, _current_context_id(ts));
    if (!it) {
        // callback functions in some circumtances, can be called before the context entry is not
        // created. (See issue 21). To prevent this problem we need to ensure the context entry for
        // the thread is always available here.
        return _profile_thread(ts);
    }
    return (_ctx *)it->val;
}

// the pit will be cleared by the relevant freelist. we do not free it here.
// we only DECREF the CodeObject or the MethodDescriptive string.
static void
_del_pit(_pit *pit)
{
    // the pit will be freed by fldestrot() in clear_stats, otherwise it stays
    // for later enumeration
    _pit_children_info *it,*next;

    // only DECR for the non-tagged main pit
    if (pit->tagged_pits) {
        Py_CLEAR(pit->name);
        Py_CLEAR(pit->modname);
    }

    // free children
    it = pit->children;
    while(it) {
        next = (_pit_children_info *)it->next;
        yfree(it);
        it = next;
    }
    pit->children = NULL;

    // free tagged_pits
    if (pit->tagged_pits) {
        henum(pit->tagged_pits, _pitenumdel, NULL);
    }
}

static PyObject *
_pycfunction_module_name(PyCFunctionObject *cfn)
{
    PyObject *obj;
    PyObject *name;

    // The __module__ attribute, can be anything
    obj = cfn->m_module;

    if (!obj) {
        // TODO: Is this always correct?
        name = PyStr_FromString("__builtin__");
    } else if (PyStr_Check(obj)) {
        Py_INCREF(obj);
        name = obj;
    } else if (PyModule_Check(obj)) {
        const char *s = PyModule_GetName(obj);
        if (!s) {
            goto error;
        }
        name = PyStr_FromString(s);
    } else {
        // Something else - str(obj)
        name = PyObject_Str(obj);
    }

    return name;

error:
    PyErr_Clear();
    return PyStr_FromString("<unknown>");
}

static _pit *
_ccode2pit(void *cco)
{
    PyCFunctionObject *cfn;
    _hitem *it;
    PyObject *name;

    cfn = cco;
    // Issue #15:
    // Hashing cfn to the pits table causes different object methods
    // to be hashed into the same slot. Use cfn->m_ml for hashing the
    // Python C functions.
    it = hfind(current_ctx->pits, (uintptr_t)cfn->m_ml);
    if (!it) {
        _pit *pit = _create_pit(0);
        if (!pit)
            return NULL;
        if (!hadd(current_ctx->pits, (uintptr_t)cfn->m_ml, (uintptr_t)pit))
            return NULL;

        pit->builtin = 1;
        pit->modname = _pycfunction_module_name(cfn);
        pit->lineno = 0;

        // built-in method?
        if (cfn->m_self != NULL) {
            name = PyStr_FromString(cfn->m_ml->ml_name);
            if (name != NULL) {
                PyObject *obj_type = PyObject_Type(cfn->m_self);
                PyObject *mo = _PyType_Lookup((PyTypeObject *)obj_type, name);
                Py_XINCREF(mo);
                Py_XDECREF(obj_type);
                Py_DECREF(name);
                if (mo != NULL) {
                    pit->name = PyObject_Repr(mo);
                    Py_DECREF(mo);
                    return pit;
                }
            }
            PyErr_Clear();
        }
        pit->name = PyStr_FromString(cfn->m_ml->ml_name);
        return pit;
    }
    return ((_pit *)it->val);
}

// maps the PyCodeObject to our internal pit item via hash table.
static _pit *
_code2pit(PyFrameObject *fobj)
{
    _hitem *it;
    PyCodeObject *cobj;
    _pit *pit;

    cobj = fobj->f_code;
    it = hfind(current_ctx->pits, (uintptr_t)cobj);
    if (it) {
        return ((_pit *)it->val);
    }

    pit = _create_pit(0);
    if (!pit)
        return NULL;
    if (!hadd(current_ctx->pits, (uintptr_t)cobj, (uintptr_t)pit))
        return NULL;

    pit->name = NULL;
    Py_INCREF(cobj->co_filename);
    pit->modname = cobj->co_filename;
    pit->lineno = cobj->co_firstlineno;

    PyFrame_FastToLocals(fobj);
    if (cobj->co_argcount) {
        const char *firstarg = PyStr_AS_CSTRING(PyTuple_GET_ITEM(cobj->co_varnames, 0));

        if (!strcmp(firstarg, "self")) {
            PyObject* locals = fobj->f_locals;
            if (locals) {
                PyObject* self = PyDict_GetItemString(locals, "self");
                if (self) {
                    PyObject *class_obj = PyObject_GetAttrString(self, "__class__");
                    if (class_obj) {
                        PyObject *class_name = PyObject_GetAttrString(class_obj, "__name__");
                        if (class_name) {
                            pit->name = PyStr_FromFormat("%s.%s", PyStr_AS_CSTRING(class_name), PyStr_AS_CSTRING(cobj->co_name));
                            Py_DECREF(class_name);
                        }
                        Py_DECREF(class_obj);
                    }
                }
            }
        }
    }
    if (!pit->name) {
        Py_INCREF(cobj->co_name);
        pit->name = cobj->co_name;
    }

    PyFrame_LocalsToFast(fobj, 0);

    return pit;
}

static _pit *
_get_frame(void)
{
    _cstackitem *ci;

    ci = shead(current_ctx->cs);
    if (!ci) {
        return NULL;
    }
    return ci->ckey;
}

static _cstackitem *
_push_frame(_pit *cp)
{
    return spush(current_ctx->cs, cp);
}

static _pit *
_pop_frame(void)
{
    _cstackitem *ci;

    ci = spop(current_ctx->cs);
    if (!ci) {
        return NULL;
    }
    return ci->ckey;
}

static _pit_children_info *
_add_child_info(_pit *parent, _pit *current)
{
    _pit_children_info *newci;

    // TODO: Optimize by moving to a freelist?
    newci = ymalloc(sizeof(_pit_children_info));
    if (!newci) {
        return NULL;
    }
    newci->index = current->index;
    newci->callcount = 0;
    newci->nonrecursive_callcount = 0;
    newci->ttotal = 0;
    newci->tsubtotal = 0;
    newci->next = (struct _pit_children_info *)parent->children;
    parent->children = (_pit_children_info *)newci;

    return newci;
}

static _pit_children_info *
_get_child_info(_pit *parent, _pit *current, int add_if_not_exists)
{
    _pit_children_info *citem;

    if (!parent || !current) {
        return NULL;
    }

    citem = parent->children;
    while(citem) {
        if (citem->index == current->index) {
            break;
        }
        citem = (_pit_children_info *)citem->next;
    }

    if (add_if_not_exists && !citem) {
        citem = _add_child_info(parent, current);
    }

    return citem;
}

static long
get_rec_level(uintptr_t key)
{
    _hitem *it;

    it = hfind(current_ctx->rec_levels, key);
    if (!it) {
        _log_err(1);
        return -1; // should not happen
    }
    return it->val;
}

static int
incr_rec_level(uintptr_t key)
{
    _hitem *it;

    it = hfind(current_ctx->rec_levels, key);
    if (it) {
        it->val++;
    } else {
        if (!hadd(current_ctx->rec_levels, key, 1))
        {
            _log_err(2);
            return 0; // should not happen
        }
    }
    return 1;
}

static int
decr_rec_level(uintptr_t key)
{
    _hitem *it;
    uintptr_t v;

    it = hfind(current_ctx->rec_levels, key);
    if (it) {
        v = it->val--;  /*supress warning -- it is safe to cast long vs pointers*/
        if (v == 0)
        {
            hfree(current_ctx->rec_levels, it);
        }
    } else {
        _log_err(3);
        return 0; // should not happen
    }
    return 1;
}

static long long
_get_frame_elapsed(void)
{
    _cstackitem *ci;
    _pit *cp;
    long long result;

    ci = shead(current_ctx->cs);
    if (!ci) {
        return 0LL;
    }
    cp = ci->ckey;

    if (test_timings) {
        uintptr_t rlevel = get_rec_level((uintptr_t)cp);
        PyObject *formatted_string = PyStr_FromFormat(
                "%s_%d", PyStr_AS_CSTRING(cp->name), rlevel);

        PyObject *tval = PyDict_GetItem(test_timings, formatted_string);
        Py_DECREF(formatted_string);
        if (tval) {
            result = PyLong_AsLong(tval);
        } else {
            result = DEFAULT_TEST_ELAPSED_TIME;
        }

    } else {
        result = tickcount() - ci->t0;
    }

    return result;
}

static int 
_coro_enter(_pit *cp, PyFrameObject *frame)
{
    _coro *coro;

    if (!(get_timing_clock_type() == WALL_CLOCK) || 
        (get_rec_level((uintptr_t)cp) != 1)) {
            return 0;
    }

    // if we already have this coro then it was yielded before
    coro = cp->coroutines;
    while(coro) {
        if (coro->frame == frame) {
            return 0;
        }
        coro = (_coro *)coro->next;
    }

    //printf("CORO ENTER %s %p\n", PyStr_AS_CSTRING(cp->name), frame);

    coro = ymalloc(sizeof(_coro));
    if (!coro) {
        return -1;
    }

    coro->frame = frame;
    coro->t0 = tickcount();
    coro->next = NULL;
    
    if (cp->coroutines) {
        coro->next = (struct _coro *)cp->coroutines;
    }
    cp->coroutines = coro;

    return 1;
}

static long long 
_coro_exit(_pit *cp, PyFrameObject *frame)
{
    _coro *coro, *prev;
    long long _t0;

    if (!(get_timing_clock_type() == WALL_CLOCK) || 
        (get_rec_level((uintptr_t)cp) != 1)) {
            return 0;
    }

    //printf("CORO EXIT %s %p\n", PyStr_AS_CSTRING(cp->name), frame);

    prev = NULL;
    coro = cp->coroutines;
    while(coro) {
        if (coro->frame == frame) {
            _t0 = coro->t0;
            if (prev) {
                prev->next = coro->next;
            } else {
                cp->coroutines = NULL;
            }
            yfree(coro);
            //printf("CORO EXIT val %s %p %llu\n", PyStr_AS_CSTRING(cp->name), frame, tickcount()-_t0);
            return tickcount() - _t0;
        }
        prev = coro;
        coro = (_coro *)coro->next;
    }

    // expected: a func leaves without enter
    return 0;
}


static void
_call_enter(PyObject *self, PyFrameObject *frame, PyObject *arg, int ccall)
{
    _pit *cp,*pp;
    _cstackitem *ci;
    _pit_children_info *pci;

    if (ccall) {
        cp = _ccode2pit((PyCFunctionObject *)arg);
    } else {
        cp = _code2pit(frame);
    }

    // something went wrong. No mem, or another error. we cannot find
    // a corresponding pit. just run away:)
    if (!cp) {
        _log_err(4);
        return;
    }

    // create/update children info if we have a valid parent
    pp = _get_frame();
    if (pp) {
        pci = _get_child_info(pp, cp, 1);
        if (!pci) {
            _log_err(12); // defensive runaway
            return;
        }
        incr_rec_level((uintptr_t)pci);
    }

    ci = _push_frame(cp);
    if (!ci) { // runaway! (defensive)
        _log_err(5);
        return;
    }

    ci->t0 = tickcount();

    incr_rec_level((uintptr_t)cp);

    // TODO: Comment
    if (IS_ASYNC(frame)) {
        _coro_enter(cp, frame);
    }
}

static void
_call_leave(PyObject *self, PyFrameObject *frame, PyObject *arg, int ccall)
{
    long long elapsed;
    _pit *cp, *pp, *ppp;
    _pit_children_info *pci,*ppci,*tpci,*tppci;
    int yielded = 0;
    _pit *tcp, *tpp, *tppp;
    uintptr_t curr_tag;
    _ctx *prev_ctx;

    tcp = tpp = tppp = NULL;
    pci = ppci = tpci = tppci = NULL;

    prev_ctx = current_ctx;
    curr_tag = _current_tag();
    current_ctx = prev_ctx;

    elapsed = _get_frame_elapsed();

    // leaving a frame while callstack is empty?
    cp = _pop_frame();
    if (!cp) {
        return;
    }

    //if (frame->f_code->co_flags & CO_GENERATOR) {
        //printf("is a generator func.\n");
    //}

    // TODO: Comment
    if (IS_ASYNC(frame)) {
        if (frame->f_stacktop) {
            yielded = 1;
            if (get_timing_clock_type() == WALL_CLOCK) {
                // In fact setting this zero means following:
                // for this specific pit, we say that if WALL_CLOCK is on,
                // we only aggregate time between first non-recursive enter and
                // last non-recursive exit
                elapsed = 0;
            }
        } else {
            long long coro_elapsed = _coro_exit(cp, frame);

            if (coro_elapsed > 0) {
                elapsed = coro_elapsed;
            }
        }
    }

    // get the tagged pit if there is any
    tcp = _get_tagged_pit(cp, curr_tag);
    if (!yielded) {
        cp->callcount++;
        if (tcp) {
            tcp->callcount++;
        }
    }

    // is this the last function in the callstack?
    pp = _pop_frame();
    if (!pp) {
        // update actual pit
        cp->ttotal += elapsed;
        cp->tsubtotal += elapsed;
        if (!yielded) {
            cp->nonrecursive_callcount++;
        }
        // update tagged pit
        if (tcp) {
            tcp->ttotal += elapsed;
            tcp->tsubtotal += elapsed;
            if (!yielded) {
                tcp->nonrecursive_callcount++;
            }
        }
        decr_rec_level((uintptr_t)cp);
        return;
    }
    tpp = _get_tagged_pit(pp, curr_tag);
    // get children info
    pci = _get_child_info(pp, cp, 0);
    if(!pci)
    {
        _log_err(6);
        return; // defensive
    }

    tpci = _get_child_info(tpp, tcp, 1);

    // a calls b. b's elapsed time is subtracted from a's tsub and 
    // a adds its own elapsed it is leaving.
    pp->tsubtotal -= elapsed;
    if (tpp) {
        tpp->tsubtotal -= elapsed;
    }
    cp->tsubtotal += elapsed;
    if (tcp) {
        tcp->tsubtotal += elapsed;
    }

    if (!yielded) {
        pci->callcount++;
        if (tpci) {
            tpci->callcount++;
        }
    }

    // a->b->c. b->c is substracted from a->b.
    ppp = _get_frame();
    if (ppp) {
        ppci = _get_child_info(ppp, pp, 0);
        if(!ppci) {
            _log_err(7);
            return;
        }

        tppp = _get_tagged_pit(ppp, curr_tag);
        tppci = _get_child_info(tppp, tpp, 1);
        ppci->tsubtotal -= elapsed;
        if (tppci) {
            tppci->tsubtotal -= elapsed;
        }
    }
    pci->tsubtotal += elapsed;
    if (tpci) {
        tpci->tsubtotal += elapsed;
    }

    // wait for the top-level function/parent/child to update timing values accordingly.
    if (get_rec_level((uintptr_t)cp) == 1) {
        cp->ttotal += elapsed;
        if (!yielded) {
            cp->nonrecursive_callcount++;
            pci->nonrecursive_callcount++;
        }
        if (tcp) {
            tcp->ttotal += elapsed;
            if (!yielded) {
                tcp->nonrecursive_callcount++;
                if (tpci) {
                    tpci->nonrecursive_callcount++;
                }
            }
        }
    }

    if (get_rec_level((uintptr_t)pci) == 1) {
        pci->ttotal += elapsed;
        if (tpci) {
            tpci->ttotal += elapsed;
        }
    }

    decr_rec_level((uintptr_t)pci);
    decr_rec_level((uintptr_t)cp);

    if (!_push_frame(pp)) {
        _log_err(8);
        return; //defensive
    }
}

static int
_pitenumdel(_hitem *item, void *arg)
{
    _del_pit((_pit *)item->val);
    return 0;
}

// context will be cleared by the free list. we do not free it here.
// we only free the context call stack.
static void
_del_ctx(_ctx * ctx)
{
    sdestroy(ctx->cs);
    htdestroy(ctx->rec_levels);
    henum(ctx->pits, _pitenumdel, NULL);
    htdestroy(ctx->pits);
    Py_CLEAR(ctx->name);
}

static int
_yapp_callback(PyObject *self, PyFrameObject *frame, int what,
               PyObject *arg)
{
    PyObject *last_type, *last_value, *last_tb;
    PyErr_Fetch(&last_type, &last_value, &last_tb);

    // profiler callback functions are not reentrant safe, 
    // while an event is in progress, do not allow another event
    // to be processed
    //while(_profile_event_running) {
    //    Py_BEGIN_ALLOW_THREADS;
    //    Py_END_ALLOW_THREADS;
        // TODO: add a timeout for this to defentsively exit
    //}

    _profile_event_running = 1;

    // get current ctx
    current_ctx = _thread2ctx(PyThreadState_GET());
    if (!current_ctx) {
        _log_err(9);
        goto finally;
    }

    // do not profile if multi-threaded is off and the context is different than
    // the context that called start.
    if (!flags.multithreaded && current_ctx != initial_ctx) {
        goto finally;
    }

    // update ctx stats
    if (prev_ctx != current_ctx) {
        current_ctx->sched_cnt++;
    }
    prev_ctx = current_ctx;
    if (!current_ctx->name)
    {
        current_ctx->name = _current_context_name();
    }

    switch (what) {
    case PyTrace_CALL:
        _call_enter(self, frame, arg, 0);
        break;
    case PyTrace_RETURN: // either normally or with an exception
        _call_leave(self, frame, arg, 0);
        break;
    /* case PyTrace_EXCEPTION:
        If the exception results in the function exiting, a
        PyTrace_RETURN event will be generated, so we don't need to
        handle it. */

    case PyTrace_C_CALL:
        if (PyCFunction_Check(arg))
            _call_enter(self, frame, arg, 1); // set ccall to true
        break;

    case PyTrace_C_RETURN:
    case PyTrace_C_EXCEPTION:
        if (PyCFunction_Check(arg))
            _call_leave(self, frame, arg, 1);
        break;
    default:
        break;
    }

    goto finally;

finally:
    if (last_type) {
        PyErr_Restore(last_type, last_value, last_tb);
    }

    // there shall be no context switch happenning inside 
    // profile events and no concurent running events is possible
    if (current_ctx->ts_ptr != PyThreadState_GET()) {
        //abort();
        _log_err(15);
    }

    _profile_event_running = 0;

    return 0;
}

static _ctx *
_profile_thread(PyThreadState *ts)
{
    uintptr_t ctx_id;
    _ctx *ctx;
    _hitem *it;

    ctx_id = _current_context_id(ts);
    it = hfind(contexts, ctx_id);
    if (!it) {
        ctx = _create_ctx();
        if (!ctx) {
            return NULL;
        }    
        if (!hadd(contexts, ctx_id, (uintptr_t)ctx)) {
            _del_ctx(ctx);
            if (!flput(flctx, ctx)) {
                _log_err(10);
            }
            _log_err(11);
            return NULL;
        }
    } else {
        ctx = (_ctx *)it->val;
    }
    
    ts->use_tracing = 1;
    ts->c_profilefunc = _yapp_callback;
    ctx->id = ctx_id;
    ctx->tid = ts->thread_id;
    ctx->ts_ptr = ts;

    return ctx;
}

static _ctx*
_unprofile_thread(PyThreadState *ts)
{
    ts->use_tracing = 0;
    ts->c_profilefunc = NULL;

    return NULL; //dummy return for enum_threads() func. prototype
}

static void
_ensure_thread_profiled(PyThreadState *ts)
{
    if (ts->c_profilefunc != _yapp_callback)
        _profile_thread(ts);
}

static void
_enum_threads(_ctx* (*f) (PyThreadState *))
{
    PyThreadState *ts;
    PyInterpreterState* is;

    for(is=PyInterpreterState_Head();is!=NULL;is = PyInterpreterState_Next(is))
    {
        for (ts=PyInterpreterState_ThreadHead(is) ; ts != NULL; ts = ts->next) {
            f(ts);
        }
    }
}

static int
_init_profiler(void)
{
    // already initialized? only after clear_stats() and first time, this flag
    // will be unset.
    if (!yappinitialized) {
        contexts = htcreate(HT_CTX_SIZE);
        if (!contexts)
            goto error;
        flpit = flcreate(sizeof(_pit), FL_PIT_SIZE);
        if (!flpit)
            goto error;
        flctx = flcreate(sizeof(_ctx), FL_CTX_SIZE);
        if (!flctx)
            goto error;
        yappinitialized = 1;
    }
    return 1;

error:
    if (contexts) {
        htdestroy(contexts);
        contexts = NULL;
    }
    if (flpit) {
        fldestroy(flpit);
        flpit = NULL;
    }
    if (flctx) {
        fldestroy(flctx);
        flctx = NULL;
    }

    return 0;
}

static PyObject*
profile_event(PyObject *self, PyObject *args)
{
    const char *ev;
    PyObject *arg;
    PyObject *event;
    PyFrameObject * frame;

    if (!PyArg_ParseTuple(args, "OOO", &frame, &event, &arg)) {
        return NULL;
    }

    if (flags.multithreaded) {
        _ensure_thread_profiled(PyThreadState_GET());
    }

    ev = PyStr_AS_CSTRING(event);

    if (strcmp("call", ev)==0)
        _yapp_callback(self, frame, PyTrace_CALL, arg);
    else if (strcmp("return", ev)==0)
        _yapp_callback(self, frame, PyTrace_RETURN, arg);
    else if (strcmp("c_call", ev)==0)
        _yapp_callback(self, frame, PyTrace_C_CALL, arg);
    else if (strcmp("c_return", ev)==0)
        _yapp_callback(self, frame, PyTrace_C_RETURN, arg);
    else if (strcmp("c_exception", ev)==0)
        _yapp_callback(self, frame, PyTrace_C_EXCEPTION, arg);

    Py_RETURN_NONE;
}

static long long
_calc_cumdiff(long long a, long long b)
{
    long long r;

    r = a - b;
    if (r < 0)
        return 0;
    return r;
}

static int
_ctxenumdel(_hitem *item, void *arg)
{
    _del_ctx(((_ctx *)item->val) );
    return 0;
}

// start profiling. return 1 on success, or 0 and set exception.
static int
_start(void)
{
    if (yapprunning)
        return 1;

    if (!_init_profiler()) {
        PyErr_SetString(YappiProfileError, "profiler cannot be initialized.");
        return 0;
    }

    if (flags.multithreaded) {
        _enum_threads(&_profile_thread);
    } else {
        _ensure_thread_profiled(PyThreadState_GET());
        initial_ctx = _thread2ctx(PyThreadState_GET());
    }

    yapprunning = 1;
    yapphavestats = 1;
    time (&yappstarttime);
    yappstarttick = tickcount();
    return 1;
}

static void
_stop(void)
{
    if (!yapprunning)
        return;

    _enum_threads(&_unprofile_thread);

    yapprunning = 0;
    yappstoptick = tickcount();

    // let's make sure this because there might be some cases
    // like TerminateThread()? cause a thread to exit in the middle
    // of a profile event. We do not want to get stuck on this.
    _profile_event_running = 0;
}

static PyObject*
clear_stats(PyObject *self, PyObject *args)
{
    PyObject *d;

    if (!yapphavestats) {
        Py_RETURN_NONE;
    }

    current_ctx = NULL;
    prev_ctx = NULL;
    initial_ctx = NULL;

    henum(contexts, _ctxenumdel, NULL);
    htdestroy(contexts);
    contexts = NULL;

    fldestroy(flpit);
    flpit = NULL;

    fldestroy(flctx);
    flctx = NULL;

    yappinitialized = 0;
    yapphavestats = 0;
    ycurfuncindex = 0;
    ycurthreadindex = 0;

    d = PyThreadState_GET()->dict;
    if (PyDict_GetItemString(d, "_yappi_tid")) {
        PyDict_DelItemString(d, "_yappi_tid");
    }

    Py_CLEAR(test_timings);

// check for mem leaks if DEBUG_MEM is specified
#ifdef DEBUG_MEM
    YMEMLEAKCHECK();
#endif

    Py_RETURN_NONE;
}

// normalizes the time count if test_timing is not set.
static double
_normt(long long tickcount)
{
    if (!test_timings) {
        return tickcount * tickfactor();
    }
    return (double)tickcount;
}

static int
_ctxenumstat(_hitem *item, void *arg)
{
    PyObject *efn;
    const char *tcname;
    _ctx *ctx;
    long long cumdiff;
    PyObject *exc;

    ctx = (_ctx *)item->val;

    if(ctx->sched_cnt == 0) {
        // we return here because if sched_cnt is zero, then this means not any single function
        // executed in the context of this thread. We do not want to show any thread stats for this case especially
        // because of the following case: start()/lots of MT calls/stop()/clear_stats()/start()/get_thread_stats()
        // still returns the threads from the previous cleared session. That is because Python VM does not free them
        // in the second start() call, we enumerate the active threads from the threading module and they are still there.
        // second invocation of test_start_flags() generates this situation.
        return 0;
    }

    if (ctx->name)
        tcname = PyStr_AS_CSTRING(ctx->name);
    else
        tcname = UNINITIALIZED_STRING_VAL;

    efn = (PyObject *)arg;

    cumdiff = _calc_cumdiff(tickcount(), ctx->t0);

    exc = PyObject_CallFunction(efn, "((skkfk))", tcname, ctx->id, ctx->tid,
        cumdiff * tickfactor(), ctx->sched_cnt);
    if (!exc) {
        PyErr_Print();
        return 1; // abort enumeration
    }

    Py_DECREF(exc);
    return 0;
}

static PyObject*
enum_thread_stats(PyObject *self, PyObject *args)
{
    PyObject *enumfn;

    if (!yapphavestats) {
        Py_RETURN_NONE;
    }

    if (!PyArg_ParseTuple(args, "O", &enumfn)) {
        PyErr_SetString(YappiProfileError, "invalid param to enum_thread_stats");
        return NULL;
    }

    if (!PyCallable_Check(enumfn)) {
        PyErr_SetString(YappiProfileError, "enum function must be callable");
        return NULL;
    }

    henum(contexts, _ctxenumstat, enumfn);

    Py_RETURN_NONE;
}

static int
_pitenumstat(_hitem *item, void *arg)
{
    _pit *pt;
    PyObject *exc;
    PyObject *children;
    _pit_children_info *pci;
    _ctxfuncenumarg *eargs;

    children = NULL;
    pt = (_pit *)item->val;
    eargs = (_ctxfuncenumarg *)arg;

    // do not show builtin pits if specified
    if  ((!flags.builtins) && (pt->builtin)) {
        return 0;
    }

    // convert children function index list to PyList
    children = PyList_New(0);
    pci = pt->children;
    while(pci) {
        PyObject *stats_tuple;
        // normalize tsubtotal. tsubtotal being negative is an expected situation.
        if (pci->tsubtotal < 0) {
            pci->tsubtotal = 0;
        }
        if (pci->callcount == 0)
            pci->callcount = 1;
        stats_tuple = Py_BuildValue("Ikkff", pci->index, pci->callcount,
                pci->nonrecursive_callcount, _normt(pci->ttotal),
                _normt(pci->tsubtotal));
        PyList_Append(children, stats_tuple);
        Py_DECREF(stats_tuple);
        pci = (_pit_children_info *)pci->next;
    }
    // normalize values
    if (pt->tsubtotal < 0)
        pt->tsubtotal = 0;
    if (pt->callcount == 0)
        pt->callcount = 1;

    exc = PyObject_CallFunction(eargs->efn, "((OOkkkIffIOkOk))", pt->name, pt->modname, pt->lineno, pt->callcount,
                        pt->nonrecursive_callcount, pt->builtin, _normt(pt->ttotal), _normt(pt->tsubtotal),
                        pt->index, children, eargs->ctx->id, eargs->ctx->name, pt->tag);
    if (!exc) {
        PyErr_Print();
        Py_XDECREF(children);
        return 1; // abort enumeration
    }

    Py_DECREF(exc);
    Py_XDECREF(children);

    // enumerate tagged pits
    if (pt->tagged_pits) {
        henum(pt->tagged_pits, _pitenumstat, arg);
    }

    return 0;
}

static int 
_ctxfuncenumstat(_hitem *item, void *arg)
{
    _ctxfuncenumarg ext_args;

    ext_args.ctx = (_ctx *)item->val; 
    ext_args.efn = (PyObject *)arg;

    henum(ext_args.ctx->pits, _pitenumstat, &ext_args);

    return 0;
}

static PyObject*
start(PyObject *self, PyObject *args)
{
    if (yapprunning)
        Py_RETURN_NONE;

    if (!PyArg_ParseTuple(args, "ii", &flags.builtins, &flags.multithreaded))
        return NULL;

    if (!_start())
        // error
        return NULL;

    Py_RETURN_NONE;
}

static PyObject*
stop(PyObject *self)
{
    _stop();
    Py_RETURN_NONE;
}

static PyObject*
enum_func_stats(PyObject *self, PyObject *args)
{
    PyObject *enumfn;

    if (!yapphavestats) {
        Py_RETURN_NONE;
    }

    if (!PyArg_ParseTuple(args, "O", &enumfn)) {
        PyErr_SetString(YappiProfileError, "invalid param to enum_func_stats");
        return NULL;
    }

    if (!PyCallable_Check(enumfn)) {
        PyErr_SetString(YappiProfileError, "enum function must be callable");
        return NULL;
    }

    henum(contexts, _ctxfuncenumstat, enumfn);

    Py_RETURN_NONE;
}

static PyObject *
is_running(PyObject *self, PyObject *args)
{
    return Py_BuildValue("i", yapprunning);
}

static PyObject *
get_mem_usage(PyObject *self, PyObject *args)
{
    return Py_BuildValue("l", ymemusage());
}

static PyObject *
set_tag_callback(PyObject *self, PyObject *args)
{
    PyObject* new_callback;

    if (!PyArg_ParseTuple(args, "O", &new_callback)) {
        return NULL;
    }

    if (new_callback == Py_None) {
        Py_CLEAR(tag_callback);
        Py_RETURN_NONE;
    } else if (!PyCallable_Check(new_callback)) {
        PyErr_SetString(PyExc_TypeError, "callback should be a function.");
        return NULL;
    }
    Py_XDECREF(tag_callback);
    Py_INCREF(new_callback);
    tag_callback = new_callback;

    Py_RETURN_NONE;
}

static PyObject *
set_context_id_callback(PyObject *self, PyObject *args)
{
    PyObject* new_callback;

    if (!PyArg_ParseTuple(args, "O", &new_callback)) {
        return NULL;
    }

    if (new_callback == Py_None) {
        Py_CLEAR(context_id_callback);
        Py_RETURN_NONE;
    } else if (!PyCallable_Check(new_callback)) {
        PyErr_SetString(PyExc_TypeError, "callback should be a function.");
        return NULL;
    }
    Py_XDECREF(context_id_callback);
    Py_INCREF(new_callback);
    context_id_callback = new_callback;

    Py_RETURN_NONE;
}

static PyObject *
set_context_name_callback(PyObject *self, PyObject *args)
{
    PyObject* new_callback;
    if (!PyArg_ParseTuple(args, "O", &new_callback)) {
        return NULL;
    }

    if (new_callback == Py_None) {
        Py_CLEAR(context_name_callback);
        Py_RETURN_NONE;
    } else if (!PyCallable_Check(new_callback)) {
        PyErr_SetString(PyExc_TypeError, "callback should be a function.");
        return NULL;
    }
    Py_XDECREF(context_name_callback);
    Py_INCREF(new_callback);
    context_name_callback = new_callback;

    Py_RETURN_NONE;
}

static PyObject *
set_test_timings(PyObject *self, PyObject *args)
{
    if (!PyArg_ParseTuple(args, "O", &test_timings)) {
        return NULL;
    }

    if (!PyDict_Check(test_timings))
    {
        PyErr_SetString(YappiProfileError, "timings should be dict.");
        return NULL;
    }
    Py_INCREF(test_timings);

    Py_RETURN_NONE;
}

static PyObject *
set_clock_type(PyObject *self, PyObject *args)
{
    clock_type_t clock_type;
    
    if (!PyArg_ParseTuple(args, "i", &clock_type)) {
        return NULL;
    }
    
    // return silently if same clock_type
    if (clock_type == get_timing_clock_type())
    {
        Py_RETURN_NONE;
    }
    
    if (yapphavestats) {
        PyErr_SetString(YappiProfileError, "clock type cannot be changed previous stats are available. clear the stats first.");
        return NULL;
    }
    
    if (!set_timing_clock_type(clock_type)) {
        PyErr_SetString(YappiProfileError, "Invalid clock type.");
        return NULL;
    }   
    
    Py_RETURN_NONE;
}

static PyObject *
get_clock_time(PyObject *self, PyObject *args)
{
    return PyFloat_FromDouble(tickfactor() * tickcount());
}

static PyObject *
get_clock_info(PyObject *self, PyObject *args)
{
    PyObject *api = NULL;
    PyObject *result = NULL;
    PyObject *resolution = NULL;
    clock_type_t clk_type;

    result = PyDict_New();
    
    clk_type = get_timing_clock_type();
    if (clk_type == WALL_CLOCK) {
#if defined(_WINDOWS)
        api = Py_BuildValue("s", "queryperformancecounter");
        resolution = Py_BuildValue("s", "100ns");    
#else
        api = Py_BuildValue("s", "gettimeofday");
        resolution = Py_BuildValue("s", "100ns");
#endif
    }  else {
#if defined(USE_CLOCK_TYPE_GETTHREADTIMES)
        api = Py_BuildValue("s", "getthreadtimes");
        resolution = Py_BuildValue("s", "100ns");
#elif defined(USE_CLOCK_TYPE_THREADINFO)
        api = Py_BuildValue("s", "threadinfo");
        resolution = Py_BuildValue("s", "1000ns");
#elif defined(USE_CLOCK_TYPE_CLOCKGETTIME)
        api = Py_BuildValue("s", "clockgettime");
        resolution = Py_BuildValue("s", "1ns");
#elif defined(USE_CLOCK_TYPE_RUSAGE)
        api = Py_BuildValue("s", "getrusage");
        resolution = Py_BuildValue("s", "1000ns");
#endif
    }

    PyDict_SetItemString(result, "api", api);
    PyDict_SetItemString(result, "resolution", resolution);

    Py_XDECREF(api);
    Py_XDECREF(resolution);
    return result;
}

static PyObject *
get_clock_type(PyObject *self, PyObject *args)
{
    clock_type_t clk_type;
    
    clk_type = get_timing_clock_type();
    if (clk_type == WALL_CLOCK) {
        return Py_BuildValue("s", "wall");
    }  else {
        return Py_BuildValue("s", "cpu");
    }
}


static PyObject*
get_start_flags(PyObject *self, PyObject *args)
{
    PyObject *result = NULL;
    PyObject *profile_builtins = NULL;
    PyObject *profile_multithread = NULL;
    
    if (!yapphavestats) {
        Py_RETURN_NONE;
    }

    profile_builtins = Py_BuildValue("i", flags.builtins);
    profile_multithread = Py_BuildValue("i", flags.multithreaded);
    result = PyDict_New();
    PyDict_SetItemString(result, "profile_builtins", profile_builtins);
    PyDict_SetItemString(result, "profile_multithread", profile_multithread);
    
    Py_XDECREF(profile_builtins);
    Py_XDECREF(profile_multithread);
    return result;
}

static PyObject*
_pause(PyObject *self, PyObject *args)
{
    if (yapprunning) {
        paused = 1;
        _stop();
    }

    Py_RETURN_NONE;
}

static PyObject*
_resume(PyObject *self, PyObject *args)
{
    if (paused)
    {
        paused = 0;        
        if (!_start())
            // error
            return NULL;
    }
    
    Py_RETURN_NONE;
}

static PyMethodDef yappi_methods[] = {
    {"start", start, METH_VARARGS, NULL},
    {"stop", (PyCFunction)stop, METH_NOARGS, NULL},
    {"enum_func_stats", enum_func_stats, METH_VARARGS, NULL},
    {"enum_thread_stats", enum_thread_stats, METH_VARARGS, NULL},
    {"clear_stats", clear_stats, METH_VARARGS, NULL},
    {"is_running", is_running, METH_VARARGS, NULL},
    {"get_clock_type", get_clock_type, METH_VARARGS, NULL},
    {"set_clock_type", set_clock_type, METH_VARARGS, NULL},
    {"get_clock_time", get_clock_time, METH_VARARGS, NULL},
    {"get_clock_info", get_clock_info, METH_VARARGS, NULL},
    {"get_mem_usage", get_mem_usage, METH_VARARGS, NULL},
    {"set_context_id_callback", set_context_id_callback, METH_VARARGS, NULL},
    {"set_tag_callback", set_tag_callback, METH_VARARGS, NULL},
    {"set_context_name_callback", set_context_name_callback, METH_VARARGS, NULL},
    {"_get_start_flags", get_start_flags, METH_VARARGS, NULL}, // for internal usage.
    {"_set_test_timings", set_test_timings, METH_VARARGS, NULL}, // for internal usage.
    {"_profile_event", profile_event, METH_VARARGS, NULL}, // for internal usage.
    {"_pause", _pause, METH_VARARGS, NULL}, // for internal usage.
    {"_resume", _resume, METH_VARARGS, NULL}, // for internal usage.
    {NULL, NULL}      /* sentinel */
};

#ifdef IS_PY3K
static struct PyModuleDef _yappi_module = {
    PyModuleDef_HEAD_INIT,
    "_yappi",
    _yappi__doc__,
    -1,
    yappi_methods,
    NULL,
    NULL,
    NULL,
    NULL
};
#endif

PyMODINIT_FUNC
#ifdef IS_PY3K
PyInit__yappi(void)
#else
init_yappi(void)
#endif
{
    PyObject *m, *d;

#ifdef IS_PY3K
    m = PyModule_Create(&_yappi_module);
    if (m == NULL)
        return NULL;
#else
    m = Py_InitModule("_yappi",  yappi_methods);
    if (m == NULL)
        return;
#endif

    d = PyModule_GetDict(m);
    YappiProfileError = PyErr_NewException("_yappi.error", NULL, NULL);
    PyDict_SetItemString(d, "error", YappiProfileError);

    // init the profiler memory and internal constants
    yappinitialized = 0;
    yapphavestats = 0;
    yapprunning = 0;
    paused = 0;
    flags.builtins = 0;
    flags.multithreaded = 0;
    test_timings = NULL;
    
    if (!_init_profiler()) {
        PyErr_SetString(YappiProfileError, "profiler cannot be initialized.");
#ifdef IS_PY3K
        return NULL;
#else
        return;
#endif
    }

#ifdef IS_PY3K
    return m;
#endif
}

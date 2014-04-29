/*
 * Fiber.cpp
 *
 *  Created on: Jul 23, 2012
 *      Author: lion
 */

#include "Fiber.h"
#include "ifs/os.h"

extern int stack_size;

namespace fibjs
{

#define FIBER_PER_CPU   3000
extern v8::Persistent<v8::Context> s_context;

static exlib::Queue<asyncEvent> g_jobs;
static exlib::IDLE_PROC s_oldIdle;
static int32_t s_cpus;
static int32_t s_fibers;

int g_tlsCurrent;
DateCache FiberBase::g_dc;

static class _fiber_init
{
public:
    _fiber_init()
    {
        if (os_base::CPUs(s_cpus) < 0)
            s_cpus = 4;
        s_fibers = 0;

        exlib::Service *pService = exlib::Service::getFiberService();

        g_tlsCurrent = pService->tlsAlloc();
        s_oldIdle = pService->onIdle(onIdle);
    }

    static void onIdle()
    {
        if (!g_jobs.empty() && (s_fibers < s_cpus * FIBER_PER_CPU))
        {
            s_fibers++;
            exlib::Service::CreateFiber(FiberBase::fiber_proc, NULL,
                                        stack_size * 1024)->Unref();
        }

        if (s_oldIdle)
            s_oldIdle();
    }
} s_fiber_init;

static class null_fiber_data: public Fiber_base
{
public:
    null_fiber_data()
    {
        Ref();
    }

    result_t join()
    {
        return 0;
    }

    result_t get_caller(obj_ptr<Fiber_base> &retVal)
    {
        return CALL_E_INVALID_CALL;
    }

} s_null;

void *FiberBase::fiber_proc(void *p)
{
    v8::Locker locker(isolate);
    v8::Isolate::Scope isolate_scope(isolate);

    v8::HandleScope handle_scope(isolate);
    v8::Context::Scope context_scope(
        v8::Local<v8::Context>::New(isolate, s_context));

    while (1)
    {
        asyncEvent *ae;

        if ((ae = g_jobs.tryget()) == NULL)
        {
            v8::Unlocker unlocker(isolate);
            ae = g_jobs.get();
        }

        if (ae == NULL)
            break;

        {
            v8::HandleScope handle_scope(isolate);
            ae->js_callback();
        }
    }

    return NULL;
}

void FiberBase::start()
{
    m_caller = (Fiber_base *) g_pService->tlsGet(g_tlsCurrent);

    if (m_caller)
    {
        v8::Local<v8::Object> co = m_caller->wrap();
        v8::Local<v8::Object> o = wrap();

        v8::Local<v8::Array> ks = co->GetPropertyNames();
        int len = ks->Length();

        v8::Local<v8::Array> ks1 = o->GetPropertyNames();
        int len1 = ks1->Length();

        int i, j;

        for (i = 0; i < len; i++)
        {
            v8::Local<v8::Value> k = ks->Get(i);

            for (j = 0; j < len1 && !ks1->Get(j)->Equals(k); j++);
            if (j == len1)
                o->Set(k, co->Get(k));
        }
    }

    g_jobs.put(this);
    Ref();
}

void JSFiber::callFunction1(v8::Local<v8::Function> func,
                            v8::Local<v8::Value> *args, int argCount,
                            v8::Local<v8::Value> &retVal)
{
    v8::TryCatch try_catch;
    retVal = func->Call(wrap(), argCount, args);
    if (try_catch.HasCaught())
    {
        v8::Local<v8::Value> err = try_catch.Exception();
        m_error = true;

        ReportException(try_catch, 0);
    }
}

void JSFiber::callFunction(v8::Local<v8::Value> &retVal)
{
    size_t i;

    std::vector<v8::Local<v8::Value> > argv;

    argv.resize(m_argv.size());
    for (i = 0; i < m_argv.size(); i++)
        argv[i] = v8::Local<v8::Value>::New(isolate, m_argv[i]);

    callFunction1(v8::Local<v8::Function>::New(isolate, m_func),
                  argv.data(), (int) argv.size(), retVal);

    if (!IsEmpty(retVal))
        m_result.Reset(isolate, retVal);
}

void JSFiber::js_callback()
{
    v8::Local<v8::Value> retVal;

    scope s(this);

    Unref();

    callFunction (retVal);

    v8::Local<v8::Object> o = wrap();

    m_quit.set();
    dispose();

    s_null.Ref();
    o->SetAlignedPointerInInternalField(0, &s_null);
}

void asyncCallBack::callback()
{
    g_jobs.put(this);
}

} /* namespace fibjs */

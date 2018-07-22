#include "node_thread.h"
#include "env-inl.h"
#include "node_internals.h"
#include <iostream>
#include <unordered_map>
#include <v8.h>
#include <uv.h>

namespace node {

namespace Thread {

using v8::Context;
using v8::Isolate;
using v8::Local;
using v8::Value;
using v8::Object;
using v8::FunctionCallback;
using v8::FunctionCallbackInfo;

unsigned int assigned_thread_id = 0;

std::vector<thread_data*> gThreadPool ;

inline std::vector<thread_data*>::iterator FindThreadPos(uv_thread_t thread) {
    for(std::vector<thread_data*>::iterator it=gThreadPool.begin();
            it!=gThreadPool.end(); it++)
    {
        if(uv_thread_equal(&(*it)->thread, &thread)) {
            return it ;
        }
    }
    return gThreadPool.end() ;
}
inline std::vector<thread_data*>::iterator FindThreadPos(unsigned int id) {
    for(std::vector<thread_data*>::iterator it=gThreadPool.begin();
            it!=gThreadPool.end(); it++) {

        if((*it)->id == id) {
            return it ;
        }
    }
    return gThreadPool.end() ;
}
bool IsValid(std::vector<thread_data*>::iterator it) {
    return it != gThreadPool.end() ;
}


thread_data * FindThread(uv_thread_t thread) {
    auto it = FindThreadPos(thread) ;
    return it==gThreadPool.end()? nullptr: (*it) ;
}
thread_data * FindThread(unsigned int id) {
    auto it = FindThreadPos(id) ;
    return it==gThreadPool.end()? nullptr: (*it) ;
}
v8::Isolate* CurrentIsolate() {
    auto tdata = FindThread(uv_thread_self()) ;
    return tdata==nullptr? nullptr: tdata->isolate ;
}


static void newthread(void* arg) {

    thread_data * tdata = (thread_data*)arg ;

    // nodejs 要求 argv 数组在连续的内存上
    char * argvdata = new char[6+tdata->scriptpath.length()+tdata->json_argv.length()] ;
    strcpy(argvdata, "node") ;
    strcpy(argvdata+5, tdata->scriptpath.data()) ;
    strcpy(argvdata+5+tdata->scriptpath.length()+1, tdata->json_argv.data()) ;
    char * argv[3] = {argvdata, argvdata+5, argvdata+5+tdata->scriptpath.length()+1 } ;

    uv_loop_init(tdata->loop) ;

    node::Start(tdata->loop, 3, argv, 0, nullptr, &tdata->isolate) ;

    uv_loop_close(tdata->loop);
    delete tdata->loop;

    std::vector<thread_data*>::iterator it = FindThreadPos(tdata->thread) ;
    gThreadPool.erase(it) ;
    delete tdata ;
}

void Run(const FunctionCallbackInfo<Value>& args) {
    
    thread_data * tdata = new thread_data ;
    tdata->loop = new uv_loop_t;
    tdata->id = assigned_thread_id ++ ;

    gThreadPool.push_back(tdata);

    if( args.Length()>=1 && args[0]->IsString() ){
        tdata->scriptpath = *(v8::String::Utf8Value(args[0]->ToString())) ;
    }
    else {
        return ;
    }

    if( args.Length()>=2 && args[1]->IsString() ){
        tdata->json_argv = *(v8::String::Utf8Value(args[1]->ToString())) ;
    }

    // 启动线程结束
    uv_thread_create(&tdata->thread, newthread, (void*)tdata);

    args.GetReturnValue().Set(tdata->id);
}

void HasNativeModuleLoaded(const FunctionCallbackInfo<Value>& args) {
    if( args.Length()<1 || !args[0]->IsString()){
        std::cerr << "bad argv" << std::endl ;
        return ;
    }
    char * moduleName = *(v8::String::Utf8Value(args[0]->ToString())) ;
    node_module* nm = node::get_addon_module(moduleName) ;

    args.GetReturnValue().Set(nm!=nullptr) ;
}

void InitNativeModule(const FunctionCallbackInfo<Value>& args) {
    if( args.Length()<2 || !args[0]->IsString() || !args[1]->IsObject()){
        std::cerr << "bad argv" << std::endl ;
        return ;
    }
    char * moduleName = *(v8::String::Utf8Value(args[0]->ToString())) ;

    node_module* nm = node::get_addon_module(moduleName) ;
    if(nm==nullptr) {
        std::cout << "unknow module name: " << moduleName << std::endl ;
        return ;
    }

    Environment* env = Environment::GetCurrent(args);
    auto context = env->context();

    Local<Object> module;
    Local<Object> exports;
    Local<Value> exports_v;
    if (!args[1]->ToObject(context).ToLocal(&module)
        || !module->Get(context, env->exports_string()).ToLocal(&exports_v)
        || !exports_v->ToObject(context).ToLocal(&exports) ) {
      return;
    }

    nm->nm_register_func ( exports, module, nm->nm_priv ) ;

    args.GetReturnValue().Set(exports) ;
}

void CurrentThreadId(const FunctionCallbackInfo<Value>& args) {
    uv_thread_t thread = uv_thread_self() ;

    thread_data * tdata = FindThread(thread) ;
    if( tdata==nullptr ) {
        args.GetReturnValue().Set(-1);
        return ;
    }

    args.GetReturnValue().Set(tdata->id);
}

struct async_data {
    thread_data * to;
    char * script;
} ;
void close_cb(uv_handle_t* handle) {
    delete (uv_async_t*)handle ;
}
void async_cb(uv_async_t* async) {

    async_data * data = (async_data*) async->data ;

    v8::HandleScope scope(data->to->isolate);
    v8::Script::Compile (
            v8::String::NewFromUtf8(data->to->isolate,data->script))->Run();

    delete[] ((async_data*)async->data)->script ;
    delete async->data ;
    async->data = nullptr ;

    // uv_close 也是一个异步操作，在回调 close_cb 中再删除 async
    uv_close((uv_handle_t*)async, close_cb);
}


void SendMessage(const FunctionCallbackInfo<Value>& args) {
    if( args.Length()<2 || !args[0]->IsInt32() || !args[1]->IsString()){
        std::cerr << "bad argv" << std::endl ;
        return ;
    }

    int id = args[0]->IntegerValue() ;
    thread_data * tdata = FindThread(id) ;
    if( tdata==nullptr ) {
        return ;
    }

    char * script = *(v8::String::Utf8Value(args[1]->ToString())) ;

    async_data * data = new async_data;
    data->to = tdata ;
    data->script = new char[ strlen(script)+1 ] ;
    strcpy(data->script, script) ;

    uv_async_t * async = new uv_async_t ;
    async->data = (void*) data ;
    uv_async_init(data->to->loop, async, async_cb);
    uv_async_send(async) ;
}

void Initialize(Local<Object> target,
                Local<Value> unused,
                Local<Context> context) {
  Environment* env = Environment::GetCurrent(context);
  env->SetMethod(target, "run", Run);
  env->SetMethod(target, "currentThreadId", CurrentThreadId);
  env->SetMethod(target, "initNativeModule", InitNativeModule);
  env->SetMethod(target, "hasNativeModuleLoaded", HasNativeModuleLoaded);
  env->SetMethod(target, "sendMessage", SendMessage);

  // 为主线程创建一个 thread_data 对象
  if( FindThread(uv_thread_self())==nullptr ) {
      thread_data * tdata = new thread_data ;
      tdata->id = assigned_thread_id ++ ;
      tdata->loop = uv_default_loop() ;
      tdata->thread = uv_thread_self() ;
      tdata->isolate = env->isolate() ;
      gThreadPool.push_back(tdata);
  }

  // hook v8::Isolate::GetCurrent() 函数
  if( v8::hookedGetterCurrentIsolate()==nullptr ) {
//      std::cout << "hookGetterCurrentIsolate()" ;
      v8::hookGetterCurrentIsolate(CurrentIsolate) ;
  }
}

}  // namespace Thread
}  // namespace node

NODE_BUILTIN_MODULE_CONTEXT_AWARE(thread, node::Thread::Initialize) ;

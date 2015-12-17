#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <iostream>

#include "include/libplatform/libplatform.h"
#include "include/v8.h"

#include "erlang_v8.h"
#include "debug.h"
#include "vm.h"

using namespace v8;
using namespace std;

class ArrayBufferAllocator : public v8::ArrayBuffer::Allocator {
    public:
        virtual void* Allocate(size_t length) {
            void* data = AllocateUninitialized(length);
            return data == NULL ? data : memset(data, 0, length);
        }
        virtual void* AllocateUninitialized(size_t length) { return malloc(length); }
        virtual void Free(void* data, size_t) { free(data); }
};

const char* ToCString(const v8::String::Utf8Value& value) {
  return *value ? *value : "<string conversion failed>";
}

void Report(Isolate* isolate, Handle<Value> response, uint8_t op) {
    uint32_t ref = 0;

    Handle<Value> input;

    if (response->IsUndefined()) {
        input = String::NewFromUtf8(isolate, "undefined");
    } else {
        input = JSONStringify(isolate, response);
    }

    String::Utf8Value utf8 (input);
    uint32_t len = utf8.length() + 1 + 4;

    cout << (uint8_t)((len >> 24) & 0xff);
    cout << (uint8_t)((len >> 16) & 0xff);
    cout << (uint8_t)((len >> 8) & 0xff);
    cout << (uint8_t)(len & 0xff);

    cout << op;

    cout << (uint8_t)((ref >> 24) & 0xff);
    cout << (uint8_t)((ref >> 16) & 0xff);
    cout << (uint8_t)((ref >> 8) & 0xff);
    cout << (uint8_t)(ref & 0xff);

    cout << *utf8;
    cout.flush();
}

void ReportOK(Isolate* isolate, Handle<Value> response) {
    Report(isolate, response, OP_OK);
}

void ReportError(Isolate* isolate, Handle<Value> response) {
    Report(isolate, WrapError(isolate, response), OP_ERROR);
}

void ReportException(Isolate* isolate, TryCatch* try_catch) {
    HandleScope handle_scope(isolate);
    Handle<Value> stack_trace = try_catch->StackTrace();

    if (stack_trace.IsEmpty()) {
        ReportError(isolate, try_catch->Exception());
    } else {
        const char* st = ToCString(String::Utf8Value(try_catch->StackTrace()));
        FTRACE("Stack: %s\n", st);
        ReportError(isolate, try_catch->StackTrace());
    }
}

Handle<Value> WrapError(Isolate* isolate, Handle<Value> exception) {
    EscapableHandleScope handle_scope(isolate);

    Local<Object> obj = Object::New(isolate);

    String::Utf8Value exception_string(exception);
    std::string from = std::string(*exception_string);

    obj->Set(String::NewFromUtf8(isolate, "error"),
             exception->ToString());

    // add line number and other fancy details ...

    return handle_scope.Escape(obj);
}

Handle<Value> JSONStringify(Isolate* isolate, Handle<Value> obj) {
    Handle<Context> context = isolate->GetCurrentContext();
    Handle<Object> global = context->Global();
    EscapableHandleScope handle_scope(isolate);

    Handle<Value> JSONValue = global->Get(String::NewFromUtf8(isolate, "JSON"));
    Handle<Object> JSON = JSONValue->ToObject();
    Handle<Function> stringify = Handle<Function>::Cast(
            JSON->Get(String::NewFromUtf8(isolate, "stringify")));

    Handle<Value> args[] = { obj };
    Local<Value> result = stringify->Call(JSON, 1, args);

    return handle_scope.Escape(result);
}

size_t PacketLength() {
    char bytes[4];
    size_t len;

    if (!cin.get(bytes[0]) || !cin.get(bytes[1]) ||
            !cin.get(bytes[2]) || !cin.get(bytes[3])) {
        return 0;
    }

    len = (((uint8_t)bytes[0] << 24) | ((uint8_t)bytes[1] << 16) |
            ((uint8_t)bytes[2] << 8) | (uint8_t)bytes[3]);

    return len;
}

bool NextPacket(Packet* packet) {
    size_t len = PacketLength();
    uint32_t ref = 0;

    if (len == 0) {
        return false;
    }

    string buf;
    buf.resize(len);

    for (int bytes_read = 0; bytes_read < len;) {                                                  
        if (!cin.read(&buf[bytes_read], len - bytes_read)) {
            return false;
        }
        bytes_read += cin.gcount();                                                                  
    }

    // extract the one-byte op code from the message and erase it from the
    // buffer.
    uint8_t op = buf.at(0);
    buf.erase(0, 1);

    ref = (((uint8_t)buf[0] << 24) | ((uint8_t)buf[1] << 16) |
            ((uint8_t)buf[2] << 8) | (uint8_t)buf[3]);

    buf.erase(0, 4);

    packet->op = op;
    packet->ref = ref;
    packet->data = buf;

    return true;
}

void* TimeoutHandler(void *arg) {
    struct TimeoutHandlerArgs *args = (struct TimeoutHandlerArgs*)arg;
    TRACE("Timeout started.\n");
    usleep(10000000);
    TRACE("After sleep,\n");

    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, 0x00);
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, 0x00);

    args->vm.TerminateExecution();
    args->vm.PumpMessageLoop();

    return NULL;
}


void Eval(VM vm, Packet* packet) {
    Isolate* isolate = vm.GetIsolate();

    HandleScope handle_scope(isolate);
    TryCatch try_catch(isolate);

    string input = packet->data;

    vm.Size();

    Handle<Context> context = vm.GetContext(packet->ref);
    Context::Scope context_scope(context);

    Handle<String> json_data = String::NewFromUtf8(isolate, input.c_str());
    Local<Object> instructions = JSON::Parse(json_data)->ToObject();

    Local<String> source_key = String::NewFromUtf8(isolate, "source");
    Local<String> source = instructions->Get(source_key)->ToString();

    String::Utf8Value lolsrc(source);
    std::cerr << "Script: " << ToCString(lolsrc) << std::endl;

    Handle<Script> script = Script::Compile(source);

    if (script.IsEmpty()) {
        assert(try_catch.HasCaught());
        ReportException(isolate, &try_catch);
    } else {
        pthread_t t;
        void *res;
        struct TimeoutHandlerArgs args = {
            vm.GetPlatform(),
            isolate,
            vm,
            (long)1
        };

        pthread_create(&t, NULL, TimeoutHandler, &args);

        Handle<Value> result = script->Run();

        pthread_cancel(t);
        pthread_join(t, &res);

        std::cerr << "Join: " << res << std::endl;

        if (result.IsEmpty()) {
            assert(try_catch.HasCaught());
            if (try_catch.Message().IsEmpty() && try_catch.StackTrace().IsEmpty()) {
                TRACE("It's a timeout!\n");
                Handle<String> tt = String::NewFromUtf8(isolate, "timeout");
                Report(isolate, tt, OP_TIMEOUT);
            } else {
                TRACE("It's a regular error\n");
                ReportException(isolate, &try_catch);
            }
            FTRACE("Replacing context: %i\n", packet->ref);
            vm.CreateContext(packet->ref);
        } else {
            ReportOK(isolate, result);
        }
    }
}

void Call(VM vm, Packet* packet) {
    Isolate* isolate = vm.GetIsolate();

    HandleScope handle_scope(isolate);
    TryCatch try_catch(isolate);

    string input = packet->data;

    Handle<Context> context = vm.GetContext(packet->ref);
    Context::Scope context_scope(context);

    Handle<String> json_data = String::NewFromUtf8(isolate, input.c_str());
    Local<Object> instructions = JSON::Parse(json_data)->ToObject();

    Handle<Object> global = context->Global();

    Local<String> function_key = String::NewFromUtf8(isolate, "function");
    Local<String> function_name = instructions->Get(function_key)->ToString();

    Local<String> args_key = String::NewFromUtf8(isolate, "args");
    Local<Value> args_value = instructions->Get(args_key);
    Local<Array> raw_args = Local<Array>::Cast(args_value);

    int len = raw_args->Length();
    Handle<Value> *args = new Handle<Value>[len];

    for (int i = 0; i < len; i++) { 
        args[i] = raw_args->Get(i);
    }

    // we cannot simply retrieve the function from the global scope as the
    // name can be something like `lol.flop['hi']`. wrapping the call in a
    // temporary function is much simpler than attempting to split the name
    // and check all the individual parts.
    Handle<String> prefix = String::NewFromUtf8(isolate,
            "function __call() { return ");
    Handle<String> suffix = String::NewFromUtf8(isolate,
            ".apply(null, arguments); }");
    Handle<String> source = String::Concat(String::Concat(prefix,
                function_name), suffix);
    Handle<Script> script = Script::Compile(source);
    Handle<Value> eval_result = script->Run();

    if (eval_result.IsEmpty()) {
        assert(try_catch.HasCaught());
        ReportException(isolate, &try_catch);
    } else {
        Local<String> fn = String::NewFromUtf8(isolate, "__call");
        Handle<Function> function = Handle<Function>::Cast(global->Get(fn));
        Handle<Value> result = function->Call(global, len, args);

        if (result.IsEmpty()) {
            assert(try_catch.HasCaught());
            ReportException(isolate, &try_catch);
        } else {
            ReportOK(isolate, result);
        }
    }
}

bool CommandLoop(VM& vm) {
    HandleScope handle_scope(vm.GetIsolate());

    bool reset = false;
    Packet packet;
    while (!reset && NextPacket(&packet)) {
        vm.Size();

        switch(packet.op) {
            case OP_EVAL:
                FTRACE("Eval in context: %i\n", packet.ref);
                Eval(vm, &packet);
                break;
            case OP_CALL:
                FTRACE("Call in context: %i\n", packet.ref);
                fflush(stderr);
                Call(vm, &packet);
                break;
            case OP_CREATE_CONTEXT:
                FTRACE("Creating context: %i\n", packet.ref);
                vm.CreateContext(packet.ref);
                break;
            case OP_DESTROY_CONTEXT:
                FTRACE("Destroying context: %i\n", packet.ref);
                vm.DestroyContext(packet.ref);
                break;
            case OP_RESET_VM:
                FTRACE("Ignoring reset", packet.ref);
                // reset = true;
                break;
        }
        vm.PumpMessageLoop();
        packet = (const Packet){ 0 };
    }
    Isolate::GetCurrent()->ContextDisposedNotification(); 
    return reset;
}

int main(int argc, char* argv[]) {
    ios_base::sync_with_stdio(false);

    V8::InitializeICU();
    Platform* platform = v8::platform::CreateDefaultPlatform();

    V8::InitializePlatform(platform);
    V8::Initialize();
    V8::SetFlagsFromCommandLine(&argc, argv, true);

    ArrayBufferAllocator allocator;
    Isolate::CreateParams create_params;
    create_params.array_buffer_allocator = &allocator;
    Isolate* isolate = Isolate::New(create_params);

    VM vm(platform, isolate, argc, argv);
    cerr << "Initial VM: " << &vm << endl;

    {
        Isolate::Scope isolate_scope(isolate);
        while (CommandLoop(vm));
    }

    isolate->Dispose();
    V8::Dispose();
    V8::ShutdownPlatform();
    delete platform;

    return 0;
}

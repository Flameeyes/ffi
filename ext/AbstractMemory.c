#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <ruby.h>
#include "rbffi.h"
#include "AbstractMemory.h"
#include "MemoryPointer.h"

static VALUE memory_put_float32(VALUE self, VALUE offset, VALUE value);
static VALUE memory_get_float32(VALUE self, VALUE offset);
static VALUE memory_put_float64(VALUE self, VALUE offset, VALUE value);
static VALUE memory_get_float64(VALUE self, VALUE offset);
static VALUE memory_put_pointer(VALUE self, VALUE offset, VALUE value);
static VALUE memory_get_pointer(VALUE self, VALUE offset);

static inline caddr_t memory_address(VALUE self);
VALUE rb_FFI_AbstractMemory_class = Qnil;
static VALUE classMemory = Qnil;

#define ADDRESS(self, offset) (memory_address((self)) + NUM2ULONG(offset))

#define NUM_OP(name, type, toNative, fromNative) \
static VALUE memory_put_##name(VALUE self, VALUE offset, VALUE value); \
static VALUE \
memory_put_##name(VALUE self, VALUE offset, VALUE value) \
{ \
    long off = NUM2LONG(offset); \
    AbstractMemory* memory = (AbstractMemory *) DATA_PTR(self); \
    type tmp = (type) toNative(value); \
    checkBounds(memory, off, sizeof(type)); \
    memcpy(memory->address + off, &tmp, sizeof(tmp)); \
    return self; \
} \
static VALUE memory_get_##name(VALUE self, VALUE offset); \
static VALUE \
memory_get_##name(VALUE self, VALUE offset) \
{ \
    long off = NUM2LONG(offset); \
    AbstractMemory* memory = (AbstractMemory *) DATA_PTR(self); \
    type tmp; \
    checkBounds(memory, off, sizeof(type)); \
    memcpy(&tmp, memory->address + off, sizeof(tmp)); \
    return fromNative(tmp); \
} \
static VALUE memory_put_array_of_##name(VALUE self, VALUE offset, VALUE ary); \
static VALUE \
memory_put_array_of_##name(VALUE self, VALUE offset, VALUE ary) \
{ \
    long count = RARRAY(ary)->len; \
    long off = NUM2LONG(offset); \
    AbstractMemory* memory = (AbstractMemory *) DATA_PTR(self); \
    caddr_t address = memory->address; \
    long i; \
    checkBounds(memory, off, count * sizeof(type)); \
    for (i = 0; i < count; i++) { \
        type tmp = (type) toNative(rb_ary_entry(ary, i)); \
        memcpy(address + off + (i * sizeof(type)), &tmp, sizeof(tmp)); \
    } \
    return self; \
} \
static VALUE memory_get_array_of_##name(VALUE self, VALUE offset, VALUE length); \
static VALUE \
memory_get_array_of_##name(VALUE self, VALUE offset, VALUE length) \
{ \
    long count = NUM2LONG(length); \
    long off = NUM2LONG(offset); \
    AbstractMemory* memory = (AbstractMemory *) DATA_PTR(self); \
    caddr_t address = memory->address; \
    long last = off + count; \
    long i; \
    checkBounds(memory, off, count * sizeof(type)); \
    VALUE retVal = rb_ary_new2(count); \
    for (i = off; i < last; ++i) { \
        type tmp; \
        memcpy(&tmp, address + (i * sizeof(type)), sizeof(tmp)); \
        rb_ary_push(retVal, fromNative(tmp)); \
    } \
    return retVal; \
}

NUM_OP(int8, int8_t, NUM2INT, INT2NUM);
NUM_OP(uint8, u_int8_t, NUM2UINT, UINT2NUM);
NUM_OP(int16, int16_t, NUM2INT, INT2NUM);
NUM_OP(uint16, u_int16_t, NUM2UINT, UINT2NUM);
NUM_OP(int32, int32_t, NUM2INT, INT2NUM);
NUM_OP(uint32, u_int32_t, NUM2UINT, UINT2NUM);
NUM_OP(int64, int64_t, NUM2LL, LL2NUM);
NUM_OP(uint64, u_int64_t, NUM2ULL, ULL2NUM);
NUM_OP(float32, float, NUM2DBL, rb_float_new);
NUM_OP(float64, double, NUM2DBL, rb_float_new);

static VALUE
memory_put_pointer(VALUE self, VALUE offset, VALUE value)
{ 
    AbstractMemory* memory = (AbstractMemory *) DATA_PTR(self);
    long off = NUM2LONG(offset);
    checkBounds(memory, off, sizeof(void *));
    if (rb_obj_is_kind_of(value, rb_FFI_MemoryPointer_class)) {        
        void* tmp = memory_address(value);
        memcpy(memory->address + off, &tmp, sizeof(tmp));
    } else if (TYPE(value) == T_NIL) {
        void* tmp = NULL;
        memcpy(memory->address + off, &tmp, sizeof(tmp));
    } else if (TYPE(value) == T_FIXNUM) {
        uintptr_t tmp = (uintptr_t) FIX2INT(value);
        memcpy(memory->address + off, &tmp, sizeof(tmp));
    } else if (TYPE(value) == T_BIGNUM) {
        uintptr_t tmp = (uintptr_t) NUM2ULL(value);
        memcpy(memory->address + off, &tmp, sizeof(tmp));
    } else {
        rb_raise(rb_eArgError, "value is not a pointer");
    }
    return self;
}

static VALUE
memory_get_pointer(VALUE self, VALUE offset)
{
    AbstractMemory* memory = (AbstractMemory *) DATA_PTR(self);
    long off = NUM2LONG(offset);
    caddr_t tmp;
    checkBounds(memory, off, sizeof(tmp));
    memcpy(&tmp, memory->address + off, sizeof(tmp));
    return rb_FFI_MemoryPointer_new(tmp);
}

static VALUE
memory_clear(VALUE self)
{
    AbstractMemory* ptr = (AbstractMemory *) DATA_PTR(self);
    memset(ptr->address, 0, ptr->size);
    return self;
}

static VALUE
memory_size(VALUE self) 
{
    return LONG2FIX(((AbstractMemory *) DATA_PTR(self))->size);
}

static VALUE
memory_get_string(int argc, VALUE* argv, VALUE self)
{
    VALUE length = Qnil, offset = Qnil;
    AbstractMemory* ptr = (AbstractMemory *) DATA_PTR(self);
    long off, len;
    int nargs = rb_scan_args(argc, argv, "11", &offset, &length);

    off = NUM2LONG(offset);    
    if (nargs > 1) {
        len = NUM2LONG(length);
    } else {        
        caddr_t end;
        checkBounds(ptr, off, 1);
        end = memchr(ptr->address + off, 0, ptr->size - off);
        len = ((end != NULL) ? end - ptr->address: ptr->size) - off;
    }
    checkBounds(ptr, off, len);
    return rb_str_new((char *) ptr->address + off, len);
}

static VALUE
memory_put_string(int argc, VALUE* argv, VALUE self)
{
    AbstractMemory* ptr = (AbstractMemory *) DATA_PTR(self);
    VALUE offset = Qnil, str = Qnil, length = Qnil;
    bool nulTerminate = true;
    long off, len;
    int nargs = rb_scan_args(argc, argv, "21", &offset, &str, &length);
    off = NUM2LONG(offset);
    len = RSTRING_LEN(str);
    if (nargs > 2 && length != Qnil) {
        len = MIN(NUM2ULONG(length), len);
        nulTerminate = false;
    }
    checkBounds(ptr, off, len);
    memcpy(ptr->address + off, RSTRING_PTR(str), len);

    if (nulTerminate) {
        char nul = '\0';
        memcpy(ptr->address + off + len, &nul, sizeof(nul));
    }
    return self;
}
static inline caddr_t
memory_address(VALUE self)
{
    return ((AbstractMemory *)DATA_PTR((self)))->address;
}

void
rb_FFI_AbstractMemory_Init()
{
    VALUE moduleFFI = rb_define_module("FFI");
    rb_FFI_AbstractMemory_class = classMemory = rb_define_class_under(moduleFFI, "AbstractMemory", rb_cObject);
#undef INT
#define INT(type) \
    rb_define_method(classMemory, "put_" #type, memory_put_##type, 2); \
    rb_define_method(classMemory, "get_" #type, memory_get_##type, 1); \
    rb_define_method(classMemory, "put_u" #type, memory_put_u##type, 2); \
    rb_define_method(classMemory, "get_u" #type, memory_get_u##type, 1); \
    rb_define_method(classMemory, "put_array_of_" #type, memory_put_array_of_##type, 2); \
    rb_define_method(classMemory, "get_array_of_" #type, memory_get_array_of_##type, 2); \
    rb_define_method(classMemory, "put_array_of_u" #type, memory_put_array_of_u##type, 2); \
    rb_define_method(classMemory, "get_array_of_u" #type, memory_get_array_of_u##type, 2);
    
    INT(int8);
    INT(int16);
    INT(int32);
    INT(int64);
    
#define ALIAS(name, old) \
    rb_define_alias(classMemory, "put_" #name, "put_" #old); \
    rb_define_alias(classMemory, "get_" #name, "get_" #old); \
    rb_define_alias(classMemory, "put_u" #name, "put_u" #old); \
    rb_define_alias(classMemory, "get_u" #name, "get_u" #old); \
    rb_define_alias(classMemory, "put_array_of_" #name, "put_array_of_" #old); \
    rb_define_alias(classMemory, "get_array_of_" #name, "get_array_of_" #old); \
    rb_define_alias(classMemory, "put_array_of_u" #name, "put_array_of_u" #old); \
    rb_define_alias(classMemory, "get_array_of_u" #name, "get_array_of_u" #old);
    
    ALIAS(char, int8);
    ALIAS(short, int16);
    ALIAS(int, int32);
    ALIAS(long_long, int64);
    
    if (sizeof(long) == 4) {
        rb_define_alias(classMemory, "put_long", "put_int32");
        rb_define_alias(classMemory, "put_ulong", "put_uint32");
        rb_define_alias(classMemory, "get_long", "get_int32");
        rb_define_alias(classMemory, "get_ulong", "get_uint32");
    } else {
        rb_define_alias(classMemory, "put_long", "put_int64");
        rb_define_alias(classMemory, "put_ulong", "put_uint64");
        rb_define_alias(classMemory, "get_long", "get_int64");
        rb_define_alias(classMemory, "get_ulong", "get_uint64");
    }
    rb_define_method(classMemory, "put_float32", memory_put_float32, 2);
    rb_define_method(classMemory, "get_float32", memory_get_float32, 1);
    rb_define_method(classMemory, "put_array_of_float32", memory_put_array_of_float32, 2);
    rb_define_method(classMemory, "get_array_of_float32", memory_get_array_of_float32, 2);
    rb_define_method(classMemory, "put_float64", memory_put_float64, 2);
    rb_define_method(classMemory, "get_float64", memory_get_float64, 1);
    rb_define_method(classMemory, "put_array_of_float64", memory_put_array_of_float64, 2);
    rb_define_method(classMemory, "get_array_of_float64", memory_get_array_of_float64, 2);
    rb_define_method(classMemory, "put_pointer", memory_put_pointer, 2);
    rb_define_method(classMemory, "get_pointer", memory_get_pointer, 1);
    rb_define_method(classMemory, "get_string", memory_get_string, -1);
    rb_define_method(classMemory, "put_string", memory_put_string, -1);
    rb_define_method(classMemory, "clear", memory_clear, 0);
    rb_define_method(classMemory, "total", memory_size, 0);
}

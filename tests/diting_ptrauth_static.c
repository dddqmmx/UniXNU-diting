/* LLVM/arm64e static storage fixture; addresses must be fixed up before use. */
struct signed_static {
    int *__ptrauth(2, 1, 42, "authenticates-null-values") value;
};
struct padded_static {
    unsigned char prefix;
    struct signed_static nested[3];
};
union signed_union {
    struct signed_static first;
    unsigned long raw;
};
struct nullable_static {
    int *__ptrauth(2, 1, 42) value;
};

#ifdef __cplusplus
extern "C" {
#endif
struct signed_static static_explicit = {0};
struct signed_static static_implicit;
struct signed_static static_array_explicit[3] = {{0}};
struct signed_static static_array_implicit[3];
struct padded_static static_padded_explicit = {0};
struct padded_static static_padded_implicit;
union signed_union static_union_explicit = {0};
union signed_union static_union_implicit;
int *__ptrauth(2, 0, 42, "authenticates-null-values") static_address_independent;
struct nullable_static static_nullable_explicit = {0};
struct nullable_static static_nullable_implicit;

int *static_local_explicit_load(void)
{
    static struct signed_static storage = {0};
    return storage.value;
}
int *static_local_implicit_load(void)
{
    static struct signed_static storage;
    return storage.value;
}
#ifdef __cplusplus
}
#endif

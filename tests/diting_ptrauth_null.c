/* LLVM/arm64e code-generation fixture; it does not run a hardware probe. */
struct signed_null {
    int *__ptrauth(2, 1, 42, "authenticates-null-values") value;
};
struct nullable {
    int *__ptrauth(2, 1, 42) value;
};

#ifdef __cplusplus
extern "C" {
#endif

extern void inspect_signed_null(struct signed_null *storage);
void signed_null_initialize(void) {
    struct signed_null storage = {0};
    inspect_signed_null(&storage);
}
void signed_null_replace(struct signed_null *storage) {
    struct signed_null replacement = {0};
    *storage = replacement;
}

void signed_null_clear(struct signed_null *storage) { storage->value = 0; }
int *signed_null_load(struct signed_null *storage) { return storage->value; }
void nullable_clear(struct nullable *storage) { storage->value = 0; }
int *nullable_load(struct nullable *storage) { return storage->value; }
void signed_to_nullable(struct nullable *destination, struct signed_null *source) {
    destination->value = source->value;
}
void nullable_to_signed(struct signed_null *destination, struct nullable *source) {
    destination->value = source->value;
}

#ifdef __cplusplus
}
#endif

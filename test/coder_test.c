/* coder_test.c
   Rémi Attab (remi.attab@gmail.com), 11 Sep 2017
   FreeBSD-style copyright and disclaimer apply
*/

#include "test.h"

#include "store.c"


// -----------------------------------------------------------------------------
// utils
// -----------------------------------------------------------------------------

static struct index *index_alloc(size_t pairs)
{
    return calloc(1, index_cap(pairs));
}


// -----------------------------------------------------------------------------
// leb128
// -----------------------------------------------------------------------------

static void check_leb128(uint64_t val)
{
    uint8_t data[10] = {0};

    {
        uint8_t *it = leb128_encode(data, val);
        size_t len = (uintptr_t) it - (uintptr_t) data;
             if (val < (1UL <<  7)) assert(len == 1);
        else if (val < (1UL << 14)) assert(len == 2);
        else if (val < (1UL << 21)) assert(len == 3);
        else if (val < (1UL << 28)) assert(len == 4);
        else if (val < (1UL << 35)) assert(len == 5);
        else if (val < (1UL << 42)) assert(len == 6);
        else if (val < (1UL << 49)) assert(len == 7);
        else if (val < (1UL << 56)) assert(len == 8);
        else if (val < (1UL << 63)) assert(len == 9);
        else                        assert(len == 10);
    }

    {
        uint8_t *it = data;
        uint64_t result = 0;
        assert(leb128_decode(&it, it + sizeof(data), &result));
        assert(val == result);
    }
}

bool test_leb128(void)
{
    check_leb128(0);
    for (size_t i = 0; i < 64; ++i) check_leb128(1UL << i);

    struct rng rng = rng_make(0);
    for (size_t i = 0; i < 64; ++i) {
        for (size_t j = 0; j < 100; ++j)
            check_leb128(rng_gen_range(&rng, 0, 1UL << i));
    }

    return true;
}


// -----------------------------------------------------------------------------
// vals
// -----------------------------------------------------------------------------

#define make_vals(...)                                          \
    ({                                                          \
        rill_val_t vals[] = { __VA_ARGS__ };                    \
        make_vals_impl(vals, sizeof(vals) / sizeof(vals[0]));   \
    })

#define make_index(...)                                 \
    ({                                                  \
        rill_val_t kvs[] = { __VA_ARGS__ };             \
        size_t len = sizeof(kvs) / sizeof(kvs[0]);      \
        struct index *index = index_alloc(len);         \
        for (size_t i = 0; i < len; ++i)                \
            index_put(index, kvs[i], 1);                \
        index;                                          \
    })

static struct vals *make_vals_impl(rill_val_t *list, size_t len)
{
    struct vals *vals = calloc(1, sizeof(struct vals) + sizeof(list[0]) * len);

    vals->len = len;
    memcpy(vals->data, list, sizeof(list[0]) * len);

    vals_compact(vals);
    return vals;
}

static void check_vals(struct rill_pairs *pairs, struct vals *exp)
{
    struct vals *vals = vals_cols_from_pairs(pairs, rill_col_b);

    assert(vals->len == exp->len);
    for (size_t i = 0; i < exp->len; ++i)
        assert(vals->data[i] == exp->data[i]);

    vals_rev_t rev = {0};
    vals_rev_make(vals, &rev);

    for (size_t i = 0; i < exp->len; ++i) {
        size_t index = vals_vtoi(&rev, exp->data[i]);
        assert(vals->data[index - 1] == exp->data[i]);
    }

    free(vals);
    free(exp);
    free(pairs);
    htable_reset(&rev);
}

static void check_vals_merge(struct vals *a, struct index *b, struct vals *exp)
{
    struct vals *result = vals_merge_from_index(a, b);

    assert(result->len == exp->len);
    for (size_t i = 0; i < exp->len; ++i)
        assert(result->data[i] == exp->data[i]);

    free(result);
    free(b);
    free(exp);
}

bool test_vals(void)
{
    check_vals(make_pair(kv(1, 10)), make_vals(10));

    check_vals(make_pair(kv(1, 10), kv(1, 10)), make_vals(10));
    check_vals(make_pair(kv(1, 10), kv(2, 10)), make_vals(10));

    check_vals(make_pair(kv(1, 10), kv(1, 20)), make_vals(10, 20));
    check_vals(make_pair(kv(1, 10), kv(2, 20)), make_vals(10, 20));

    check_vals(make_pair(kv(2, 20), kv(1, 10)), make_vals(10, 20));
    check_vals(make_pair(kv(1, 20), kv(1, 10)), make_vals(10, 20));

    check_vals_merge(make_vals(10), make_index(10), make_vals(10));
    check_vals_merge(make_vals(10), make_index(20), make_vals(10, 20));

    check_vals_merge(NULL, make_index(10, 20), make_vals(10, 20));
    check_vals_merge(make_vals(10, 20), make_index(20), make_vals(10, 20));
    check_vals_merge(make_vals(10, 20), make_index(20, 30), make_vals(10, 20, 30));
    check_vals_merge(make_vals(10, 20), make_index(20, 30, 40, 50, 60),
                     make_vals(10, 20, 30, 40, 50, 60));

    return true;
}


// -----------------------------------------------------------------------------
// coder
// -----------------------------------------------------------------------------

void check_coder(struct rill_pairs *pairs)
{
    rill_pairs_compact(pairs);

    struct rill_pairs *inverted = rill_pairs_new(pairs->len);
    for (size_t i = 0; i < pairs->len; ++i)
        rill_pairs_push(inverted, pairs->data[i].val, pairs->data[i].key);
    rill_pairs_compact(inverted);

    struct vals *vals_a = vals_cols_from_pairs(pairs, rill_col_b);
    struct vals *vals_b = vals_cols_from_pairs(inverted, rill_col_b);

    const size_t pairs_a_cap = coder_cap(vals_a->len, pairs->len);
    const size_t pairs_b_cap = coder_cap(vals_b->len, inverted->len);

    size_t cap = pairs_a_cap + pairs_b_cap;
    uint8_t *buffer = calloc(1, cap);
    struct index *index_a = index_alloc(vals_b->len);
    struct index *index_b = index_alloc(vals_a->len);

    size_t len = 0, len_a = 0, len_b = 0;
    {
        struct encoder coder_a =
            make_encoder(buffer, buffer + cap, vals_a, index_a);

        for (size_t i = 0; i < pairs->len; ++i)
            assert(coder_encode(&coder_a, &pairs->data[i]));
        assert(coder_finish(&coder_a));

        len_a = len = coder_a.it - buffer;
        assert(len <= pairs_a_cap);

        struct encoder coder_b =
            make_encoder(buffer + len_a, buffer + cap, vals_b, index_b);
        for (size_t i = 0; i < inverted->len; ++i)
            assert(coder_encode(&coder_b, &inverted->data[i]));
        assert(coder_finish(&coder_b));

        len_b = coder_b.it - coder_a.it;
        assert(len_b <= pairs_b_cap);

        len = coder_b.it - buffer;
        coder_close(&coder_a);
        coder_close(&coder_b);
    }

    if (false) { // hex dump for debuging
        rill_pairs_print(pairs);
        printf("buffer: start=%p, len=%lu(%lu, %lu)\n", (void *) buffer, len, len_a, len_b);
        for (size_t i = 0; i < cap;) {
            printf("%6p: ", (void *) i);
            for (size_t j = 0; j < 16 && i < cap; ++i, ++j) {
                if (j % 2 == 0) printf(" ");
                printf("%02x", buffer[i]);
            }
            printf("\n");
        }

        printf("index_a: [ ");
        for (size_t i = 0; i < index_a->len; ++i) {
            struct index_kv *kv = &index_a->data[i];
            printf("{%p, %p} ", (void *) kv->key, (void *) kv->off);
        }
        printf("]\n");

        printf("index_b: [ ");
        for (size_t i = 0; i < index_b->len; ++i) {
            struct index_kv *kv = &index_b->data[i];
            printf("{%p, %p} ", (void *) kv->key, (void *) kv->off);
        }
        printf("]\n");
    }

    { /* Coder A */
        uint8_t *start = buffer;
        struct decoder coder =
            make_decoder_at(start,
                            start + len_a,
                            index_b, index_a, 0);

        struct rill_kv kv = {0};
        for (size_t i = 0; i < pairs->len; ++i) {
            assert(coder_decode(&coder, &kv));
            assert(rill_kv_cmp(&kv, &pairs->data[i]) == 0);
        }

        assert(coder_decode(&coder, &kv));
        assert(rill_kv_nil(&kv));
    }

    { /* Coder B */
        uint8_t *start = buffer + len_a;
        struct decoder coder =
            make_decoder_at(start,
                            start + len_b,
                            index_a, index_b, 0);

        struct rill_kv kv = {0};
        for (size_t i = 0; i < pairs->len; ++i) {
            assert(coder_decode(&coder, &kv));
            assert(rill_kv_cmp(&kv, &inverted->data[i]) == 0);
        }

        assert(coder_decode(&coder, &kv));
        assert(rill_kv_nil(&kv));
    }

    { /* Decode A */
        for (size_t i = 0; i < pairs->len; ++i) {
            size_t key_idx = 0;
            uint64_t off = 0;

            assert(index_find(index_a, pairs->data[i].key, &key_idx, &off));

            uint8_t *start = buffer;
            struct decoder coder = make_decoder_at(
                start + off, start + len_a, index_b, index_a, key_idx);

            struct rill_kv kv = {0};
            do {
                assert(coder_decode(&coder, &kv));
                assert(kv.key == pairs->data[i].key);
            } while (kv.val != pairs->data[i].val);
        }
    }

    { /* Decode B */
        for (size_t i = 0; i < inverted->len; ++i) {
            size_t key_idx = 0;
            uint64_t off = 0;

            assert(index_find(index_b, inverted->data[i].key, &key_idx, &off));

            uint8_t *start = buffer + len_a;
            struct decoder coder = make_decoder_at(
                start + off, start + len_b,
                index_a, index_b,
                key_idx);

            struct rill_kv kv = {0};
            do {
                assert(coder_decode(&coder, &kv));
                assert(kv.key && kv.val);
                assert(kv.key == inverted->data[i].key);
            } while (kv.val != inverted->data[i].val);
        }
    }

    free(buffer);
    free(index_a);
    free(index_b);
    free(vals_a);
    free(vals_b);
    free(pairs);
    free(inverted);
}


bool test_coder(void)
{
    check_coder(make_pair(kv(1, 10)));
    check_coder(make_pair(kv(1, 10), kv(1, 20)));
    check_coder(make_pair(kv(1, 10), kv(2, 20)));
    check_coder(make_pair(kv(1, 10), kv(1, 20), kv(2, 30)));
    check_coder(make_pair(kv(1, 10), kv(1, 20), kv(2, 10)));

    struct rng rng = rng_make(0);
    for (size_t iterations = 0; iterations < 100; ++iterations)
        check_coder(make_rng_pairs(&rng));

    return true;
}


// -----------------------------------------------------------------------------
// main
// -----------------------------------------------------------------------------

int main(int argc, char **argv)
{
    (void) argc, (void) argv;
    bool ret = true;

    ret = ret && test_leb128();
    ret = ret && test_vals();
    ret = ret && test_coder();

    return ret ? 0 : 1;
}

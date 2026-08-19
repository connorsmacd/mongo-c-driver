// Sample code implied from the example:

#include <mongoc/mongoc.h>

#include <unistd.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// Returns a seed for `rand_r`.
static unsigned int *
get_seed(void)
{
  static unsigned int seed = 0;
  if (seed == 0) {
    seed = (unsigned int)time(NULL);
  }
  return &seed;
}

// Print each document returned by the find operation.
static void
process_result(const bson_t *result)
{
  char *json = bson_as_relaxed_extended_json(result, NULL);
  printf("%s\n", json);
  bson_free(json);
}

// Start of example code:

// Step 1: Detect overload errors

const char *retryable_error_label = "RetryableError";
const char *system_overload_error = "SystemOverloadedError";

static bool
is_system_overloaded_error(const bson_t *error_reply)
{
  return mongoc_error_has_label(error_reply, system_overload_error);
}

// Step 2: Implement operation "retry" logic using exponential backoff and jitter

const double BASE_BACKOFF_MS = 100.0;
const double MAX_BACKOFF_MS = 10000.0;
const int MAX_ATTEMPTS_DEFAULT = 2;

static double
calculate_exponential_backoff(int attempt, double base_backoff_ms)
{
  unsigned int *seed = get_seed();
  double rand01 = rand_r(seed) / (double)RAND_MAX;
  return rand01 * BSON_MIN(MAX_BACKOFF_MS, base_backoff_ms * pow(2, attempt));
}

// get_base_backoff returns the base backoff to apply for an overload error. A server may attach a positive
// `baseBackoffMS` to the error to replace the default base backoff.
static double
get_base_backoff(const bson_t *error_reply)
{
  bson_iter_t iter;
  if (bson_iter_init_find(&iter, error_reply, "baseBackoffMS") && BSON_ITER_HOLDS_INT(&iter)) {
    int64_t base_backoff_ms = bson_iter_as_int64(&iter);
    if (base_backoff_ms > 0) {
      return (double)base_backoff_ms;
    }
  }
  return BASE_BACKOFF_MS;
}

// retryable_fn returns false and sets `error_reply` on error.
typedef bool (*retryable_fn)(void *ctx, bson_t *error_reply);

static bool
execute_with_retries(retryable_fn fn, void *ctx, int max_attempts)
{
  double base_backoff_ms = BASE_BACKOFF_MS;

  for (int attempt = 0; attempt < max_attempts; attempt++) {
    bool is_retry = attempt > 0;

    if (is_retry) {
      double delay = calculate_exponential_backoff(attempt, base_backoff_ms);
      usleep((useconds_t)(delay * 1000)); // Convert ms to microseconds
    }

    bson_t error_reply = BSON_INITIALIZER;
    bool ok = fn(ctx, &error_reply);
    if (ok) {
      bson_destroy(&error_reply);
      return true;
    } else {
      bool is_retryable_overload_error =
        is_system_overloaded_error(&error_reply) && mongoc_error_has_label(&error_reply, retryable_error_label);
      bool can_retry = is_retryable_overload_error && attempt + 1 < max_attempts;

      // Apply the server-requested base backoff, if any, to the next attempt's delay.
      base_backoff_ms = get_base_backoff(&error_reply);
      bson_destroy(&error_reply);

      if (!can_retry) {
        return false;
      }
    }
  }
  return false;
}

// Step 3: Use the retry helper for collection operations

static bool
do_find(void *ctx, bson_t *error_reply)
{
  mongoc_collection_t *users_collection = (mongoc_collection_t *)ctx;

  // Find and process results:
  const bson_t *result;
  bson_t filter = BSON_INITIALIZER;
  mongoc_cursor_t *cursor = mongoc_collection_find_with_opts(users_collection, &filter, NULL, NULL);
  while (mongoc_cursor_next(cursor, &result)) {
    process_result(result);
  }

  // Check for error:
  const bson_t *error_reply_local;
  if (mongoc_cursor_error_document(cursor, NULL, &error_reply_local)) {
    if (error_reply) {
      bson_destroy(error_reply);
      bson_copy_to(error_reply_local, error_reply);
    }
    mongoc_cursor_destroy(cursor);
    return false;
  }

  mongoc_cursor_destroy(cursor);
  bson_destroy(&filter);
  return true;
}

int
main(void)
{
  mongoc_init();

  mongoc_client_t *client = mongoc_client_new("mongodb://localhost:27017");
  mongoc_collection_t *coll = mongoc_client_get_collection(client, "db", "users");

  // Original:
  do_find(coll, NULL);
  // With retry:
  execute_with_retries(do_find, coll, MAX_ATTEMPTS_DEFAULT);

  mongoc_collection_destroy(coll);
  mongoc_client_destroy(client);
  mongoc_cleanup();
}

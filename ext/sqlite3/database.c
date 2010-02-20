#include <sqlite3_ruby.h>

#define REQUIRE_OPEN_DB(_ctxt) \
  if(!_ctxt->db) \
    rb_raise(rb_path2class("SQLite3::Exception"), "cannot use a closed database");

VALUE cSqlite3Database;

static void deallocate(void * ctx)
{
  sqlite3RubyPtr c = (sqlite3RubyPtr)ctx;
  sqlite3 * db     = c->db;
  sqlite3_stmt * stmt;

  if(db) {
    while((stmt = sqlite3_next_stmt(db, NULL)) != NULL) {
      sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
  }
  xfree(c);
}

static VALUE allocate(VALUE klass)
{
  sqlite3RubyPtr ctx = xcalloc((size_t)1, sizeof(sqlite3Ruby));
  return Data_Wrap_Struct(klass, NULL, deallocate, ctx);
}

/* call-seq: SQLite3::Database.new(file, options = {})
 *
 * Create a new Database object that opens the given file. If utf16
 * is +true+, the filename is interpreted as a UTF-16 encoded string.
 *
 * By default, the new database will return result rows as arrays
 * (#results_as_hash) and has type translation disabled (#type_translation=).
 */
static VALUE initialize(int argc, VALUE *argv, VALUE self)
{
  sqlite3RubyPtr ctx;
  Data_Get_Struct(self, sqlite3Ruby, ctx);

  VALUE file;
  VALUE opts;
  VALUE zvfs;

  rb_scan_args(argc, argv, "12", &file, &opts, &zvfs);
  if(NIL_P(opts)) opts = rb_hash_new();

  int status;

#ifdef HAVE_RUBY_ENCODING_H
  if(UTF16_LE_P(file)) {
    status = sqlite3_open16(StringValuePtr(file), &ctx->db);
  } else {
#endif

    if(Qtrue == rb_hash_aref(opts, ID2SYM(rb_intern("utf16")))) {
      status = sqlite3_open16(StringValuePtr(file), &ctx->db);
    } else {

#ifdef HAVE_RUBY_ENCODING_H
      if(!UTF8_P(file)) {
        file = rb_str_export_to_enc(file, rb_utf8_encoding());
      }
#endif

      status = sqlite3_open_v2(
          StringValuePtr(file),
          &ctx->db,
          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
          NIL_P(zvfs) ? NULL : StringValuePtr(zvfs)
      );
    }

#ifdef HAVE_RUBY_ENCODING_H
  }
#endif

  CHECK(ctx->db, status);

  rb_iv_set(self, "@tracefunc", Qnil);
  rb_iv_set(self, "@authorizer", Qnil);
  rb_iv_set(self, "@encoding", Qnil);
  rb_iv_set(self, "@busy_handler", Qnil);
  rb_iv_set(self, "@results_as_hash", rb_hash_aref(opts, ID2SYM(rb_intern("results_as_hash"))));
  rb_iv_set(self, "@type_translation", rb_hash_aref(opts, ID2SYM(rb_intern("type_translation"))));

  if(rb_block_given_p()) {
    rb_yield(self);
    rb_funcall(self, rb_intern("close"), 0);
  }

  return self;
}

/* call-seq: db.close
 *
 * Closes this database.
 */
static VALUE sqlite3_rb_close(VALUE self)
{
  sqlite3RubyPtr ctx;
  Data_Get_Struct(self, sqlite3Ruby, ctx);

  sqlite3 * db = ctx->db;
  CHECK(db, sqlite3_close(ctx->db));

  ctx->db = NULL;

  return self;
}

/* call-seq: db.closed?
 *
 * Returns +true+ if this database instance has been closed (see #close).
 */
static VALUE closed_p(VALUE self)
{
  sqlite3RubyPtr ctx;
  Data_Get_Struct(self, sqlite3Ruby, ctx);

  if(!ctx->db) return Qtrue;

  return Qfalse;
}

/* call-seq: total_changes
 *
 * Returns the total number of changes made to this database instance
 * since it was opened.
 */
static VALUE total_changes(VALUE self)
{
  sqlite3RubyPtr ctx;
  Data_Get_Struct(self, sqlite3Ruby, ctx);
  REQUIRE_OPEN_DB(ctx);

  return INT2NUM((long)sqlite3_total_changes(ctx->db));
}

static void tracefunc(void * data, const char *sql)
{
  VALUE self = (VALUE)data;
  VALUE thing = rb_iv_get(self, "@tracefunc");
  rb_funcall(thing, rb_intern("call"), 1, rb_str_new2(sql));
}

/* call-seq:
 *    trace { |sql| ... }
 *    trace(Class.new { def call sql; end }.new)
 *
 * Installs (or removes) a block that will be invoked for every SQL
 * statement executed. The block receives one parameter: the SQL statement
 * executed. If the block is +nil+, any existing tracer will be uninstalled.
 */
static VALUE trace(int argc, VALUE *argv, VALUE self)
{
  sqlite3RubyPtr ctx;
  Data_Get_Struct(self, sqlite3Ruby, ctx);
  REQUIRE_OPEN_DB(ctx);

  VALUE block;

  rb_scan_args(argc, argv, "01", &block);

  if(NIL_P(block) && rb_block_given_p()) block = rb_block_proc();

  rb_iv_set(self, "@tracefunc", block);

  sqlite3_trace(ctx->db, NIL_P(block) ? NULL : tracefunc, (void *)self);

  return self;
}

static int rb_sqlite3_busy_handler(void * ctx, int count)
{
  VALUE self = (VALUE)(ctx);
  VALUE handle = rb_iv_get(self, "@busy_handler");
  VALUE result = rb_funcall(handle, rb_intern("call"), 1, INT2NUM((long)count));

  if(Qfalse == result) return 0;

  return 1;
}

/* call-seq:
 *    busy_handler { |count| ... }
 *    busy_handler(Class.new { def call count; end }.new)
 *
 * Register a busy handler with this database instance. When a requested
 * resource is busy, this handler will be invoked. If the handler returns
 * +false+, the operation will be aborted; otherwise, the resource will
 * be requested again.
 *
 * The handler will be invoked with the name of the resource that was
 * busy, and the number of times it has been retried.
 *
 * See also the mutually exclusive #busy_timeout.
 */
static VALUE busy_handler(int argc, VALUE *argv, VALUE self)
{
  sqlite3RubyPtr ctx;
  Data_Get_Struct(self, sqlite3Ruby, ctx);
  REQUIRE_OPEN_DB(ctx);

  VALUE block;

  rb_scan_args(argc, argv, "01", &block);

  if(NIL_P(block) && rb_block_given_p()) block = rb_block_proc();

  rb_iv_set(self, "@busy_handler", block);

  int status = sqlite3_trace(
      ctx->db, NIL_P(block) ? NULL : rb_sqlite3_busy_handler, (void *)self);

  CHECK(ctx->db, status);

  return self;
}

/* call-seq: last_insert_row_id
 *
 * Obtains the unique row ID of the last row to be inserted by this Database
 * instance.
 */
static VALUE last_insert_row_id(VALUE self)
{
  sqlite3RubyPtr ctx;
  Data_Get_Struct(self, sqlite3Ruby, ctx);
  REQUIRE_OPEN_DB(ctx);

  return LONG2NUM(sqlite3_last_insert_rowid(ctx->db));
}

static VALUE sqlite3val2rb(sqlite3_value * val)
{
  switch(sqlite3_value_type(val)) {
    case SQLITE_INTEGER:
      return LONG2NUM(sqlite3_value_int64(val));
      break;
    case SQLITE_FLOAT:
      return rb_float_new(sqlite3_value_double(val));
      break;
    case SQLITE_TEXT:
      return rb_tainted_str_new2((const char *)sqlite3_value_text(val));
      break;
    case SQLITE_BLOB:
      return rb_tainted_str_new2((const char *)sqlite3_value_blob(val));
      break;
    case SQLITE_NULL:
      return Qnil;
      break;
    default:
      rb_raise(rb_eRuntimeError, "bad type"); // FIXME
  }
}

static void set_sqlite3_func_result(sqlite3_context * ctx, VALUE result)
{
  switch(TYPE(result)) {
    case T_NIL:
      sqlite3_result_null(ctx);
      break;
    case T_FIXNUM:
      sqlite3_result_int64(ctx, NUM2LONG(result));
      break;
    case T_FLOAT:
      sqlite3_result_double(ctx, NUM2DBL(result));
      break;
    case T_STRING:
      sqlite3_result_text(
          ctx,
          StringValuePtr(result),
          RSTRING_LEN(result),
          SQLITE_TRANSIENT
      );
      break;
    default:
      rb_raise(rb_eRuntimeError, "can't return %s",
          rb_class2name(CLASS_OF(result)));
  }
}

static void rb_sqlite3_func(sqlite3_context * ctx, int argc, sqlite3_value **argv)
{
  VALUE callable = (VALUE)sqlite3_user_data(ctx);
  VALUE * params = xcalloc((size_t)argc, sizeof(VALUE *));
  int i;
  for(i = 0; i < argc; i++) {
    params[i] = sqlite3val2rb(argv[i]);
  }
  xfree(params);

  VALUE result = rb_funcall2(callable, rb_intern("call"), argc, params);
  set_sqlite3_func_result(ctx, result);
}

#ifndef HAVE_RB_PROC_ARITY
int rb_proc_arity(VALUE self)
{
  return (int)NUM2INT(rb_funcall(self, rb_intern("arity"), 0));
}
#endif

/* call-seq: define_function(name) { |args,...| }
 *
 * Define a function named +name+ with +args+.  The arity of the block
 * will be used as the arity for the function defined.
 */
static VALUE define_function(VALUE self, VALUE name)
{
  sqlite3RubyPtr ctx;
  Data_Get_Struct(self, sqlite3Ruby, ctx);
  REQUIRE_OPEN_DB(ctx);

  VALUE block = rb_block_proc();

  int status = sqlite3_create_function(
    ctx->db,
    StringValuePtr(name),
    rb_proc_arity(block),
    SQLITE_UTF8,
    (void *)block,
    rb_sqlite3_func,
    NULL,
    NULL
  );

  CHECK(ctx->db, status);

  return self;
}

static int sqlite3_obj_method_arity(VALUE obj, ID id)
{
  VALUE method = rb_funcall(obj, rb_intern("method"), 1, ID2SYM(id));
  VALUE arity  = rb_funcall(method, rb_intern("arity"), 0);

  return (int)NUM2INT(arity);
}

static void rb_sqlite3_step(sqlite3_context * ctx, int argc, sqlite3_value **argv)
{
  VALUE callable = (VALUE)sqlite3_user_data(ctx);
  VALUE * params = xcalloc((size_t)argc, sizeof(VALUE *));
  int i;
  for(i = 0; i < argc; i++) {
    params[i] = sqlite3val2rb(argv[i]);
  }
  xfree(params);
  rb_funcall2(callable, rb_intern("step"), argc, params);
}

static void rb_sqlite3_final(sqlite3_context * ctx)
{
  VALUE callable = (VALUE)sqlite3_user_data(ctx);
  VALUE result = rb_funcall(callable, rb_intern("finalize"), 0);
  set_sqlite3_func_result(ctx, result);
}

/* call-seq: define_aggregator(name, aggregator)
 *
 * Define an aggregate function named +name+ using the object +aggregator+.
 * +aggregator+ must respond to +step+ and +finalize+.  +step+ will be called
 * with row information and +finalize+ must return the return value for the
 * aggregator function.
 */
static VALUE define_aggregator(VALUE self, VALUE name, VALUE aggregator)
{
  sqlite3RubyPtr ctx;
  Data_Get_Struct(self, sqlite3Ruby, ctx);
  REQUIRE_OPEN_DB(ctx);

  int arity = sqlite3_obj_method_arity(aggregator, rb_intern("step"));

  int status = sqlite3_create_function(
    ctx->db,
    StringValuePtr(name),
    arity,
    SQLITE_UTF8,
    (void *)aggregator,
    NULL,
    rb_sqlite3_step,
    rb_sqlite3_final
  );

  rb_iv_set(self, "@agregator", aggregator);

  CHECK(ctx->db, status);

  return self;
}

/* call-seq: interrupt
 *
 * Interrupts the currently executing operation, causing it to abort.
 */
static VALUE interrupt(VALUE self)
{
  sqlite3RubyPtr ctx;
  Data_Get_Struct(self, sqlite3Ruby, ctx);
  REQUIRE_OPEN_DB(ctx);

  sqlite3_interrupt(ctx->db);

  return self;
}

/* call-seq: errmsg
 *
 * Return a string describing the last error to have occurred with this
 * database.
 */
static VALUE errmsg(VALUE self)
{
  sqlite3RubyPtr ctx;
  Data_Get_Struct(self, sqlite3Ruby, ctx);
  REQUIRE_OPEN_DB(ctx);

  return rb_str_new2(sqlite3_errmsg(ctx->db));
}

/* call-seq: errcode
 *
 * Return an integer representing the last error to have occurred with this
 * database.
 */
static VALUE errcode(VALUE self)
{
  sqlite3RubyPtr ctx;
  Data_Get_Struct(self, sqlite3Ruby, ctx);
  REQUIRE_OPEN_DB(ctx);

  return INT2NUM((long)sqlite3_errcode(ctx->db));
}

/* call-seq: complete?(sql)
 *
 * Return +true+ if the string is a valid (ie, parsable) SQL statement, and
 * +false+ otherwise.
 */
static VALUE complete_p(VALUE self, VALUE sql)
{
  if(sqlite3_complete(StringValuePtr(sql)))
    return Qtrue;

  return Qfalse;
}

/* call-seq: changes
 *
 * Returns the number of changes made to this database instance by the last
 * operation performed. Note that a "delete from table" without a where
 * clause will not affect this value.
 */
static VALUE changes(VALUE self)
{
  sqlite3RubyPtr ctx;
  Data_Get_Struct(self, sqlite3Ruby, ctx);
  REQUIRE_OPEN_DB(ctx);

  return INT2NUM(sqlite3_changes(ctx->db));
}

static int rb_sqlite3_auth(
    void *ctx,
    int _action,
    const char * _a,
    const char * _b,
    const char * _c,
    const char * _d)
{
  VALUE self   = (VALUE)ctx;
  VALUE action = INT2NUM(_action);
  VALUE a      = _a ? rb_str_new2(_a) : Qnil;
  VALUE b      = _b ? rb_str_new2(_b) : Qnil;
  VALUE c      = _c ? rb_str_new2(_c) : Qnil;
  VALUE d      = _d ? rb_str_new2(_d) : Qnil;
  VALUE callback = rb_iv_get(self, "@authorizer");
  VALUE result = rb_funcall(callback, rb_intern("call"), 5, action, a, b, c, d);

  if(T_FIXNUM == TYPE(result)) return (int)NUM2INT(result);
  if(Qtrue == result) return SQLITE_OK;
  if(Qfalse == result) return SQLITE_DENY;

  return SQLITE_IGNORE;
}

/* call-seq: set_authorizer = auth
 *
 * Set the authorizer for this database.  +auth+ must respond to +call+, and
 * +call+ must take 5 arguments.
 *
 * Installs (or removes) a block that will be invoked for every access
 * to the database. If the block returns 0 (or +true+), the statement
 * is allowed to proceed. Returning 1 or false causes an authorization error to
 * occur, and returning 2 or nil causes the access to be silently denied.
 */
static VALUE set_authorizer(VALUE self, VALUE authorizer)
{
  sqlite3RubyPtr ctx;
  Data_Get_Struct(self, sqlite3Ruby, ctx);
  REQUIRE_OPEN_DB(ctx);

  int status = sqlite3_set_authorizer(
      ctx->db, NIL_P(authorizer) ? NULL : rb_sqlite3_auth, (void *)self
  );

  CHECK(ctx->db, status);

  rb_iv_set(self, "@authorizer", authorizer);

  return self;
}

/* call-seq: db.busy_timeout = ms
 *
 * Indicates that if a request for a resource terminates because that
 * resource is busy, SQLite should sleep and retry for up to the indicated
 * number of milliseconds. By default, SQLite does not retry
 * busy resources. To restore the default behavior, send 0 as the
 * +ms+ parameter.
 *
 * See also the mutually exclusive #busy_handler.
 */
static VALUE set_busy_timeout(VALUE self, VALUE timeout)
{
  sqlite3RubyPtr ctx;
  Data_Get_Struct(self, sqlite3Ruby, ctx);
  REQUIRE_OPEN_DB(ctx);

  CHECK(ctx->db, sqlite3_busy_timeout(ctx->db, (int)NUM2INT(timeout)));

  return self;
}

/* call-seq: db.copy_to(target)
 *
 * Copy database to +target+
 */
static VALUE copy_to(VALUE self, VALUE _target)
{
  sqlite3RubyPtr ctx;
  sqlite3RubyPtr target;

  Data_Get_Struct(self, sqlite3Ruby, ctx);
  Data_Get_Struct(_target, sqlite3Ruby, target);

  REQUIRE_OPEN_DB(ctx);
  REQUIRE_OPEN_DB(target);

  sqlite3_backup * b = sqlite3_backup_init(target->db, "main", ctx->db, "main");

  if(b) {
    sqlite3_backup_step(b, -1);
    sqlite3_backup_finish(b);
  }
  CHECK(target->db, sqlite3_errcode(target->db));

  return self;
}

#ifdef HAVE_RUBY_ENCODING_H
static int enc_cb(void * _self, int columns, char **data, char **names)
{
  VALUE self = (VALUE)_self;

  int index = rb_enc_find_index(data[0]);
  rb_encoding * e = rb_enc_from_index(index);
  rb_iv_set(self, "@encoding", rb_enc_from_encoding(e));

  return 0;
}

/* call-seq: db.encoding
 *
 * Fetch the encoding set on this database
 */
static VALUE db_encoding(VALUE self)
{
  sqlite3RubyPtr ctx;
  Data_Get_Struct(self, sqlite3Ruby, ctx);
  REQUIRE_OPEN_DB(ctx);

  VALUE enc = rb_iv_get(self, "@encoding");

  if(NIL_P(enc)) {
    sqlite3_exec(ctx->db, "PRAGMA encoding", enc_cb, (void *)self, NULL);
  }

  return rb_iv_get(self, "@encoding");
}
#endif

void init_sqlite3_database()
{
  cSqlite3Database = rb_define_class_under(mSqlite3, "Database", rb_cObject);

  rb_define_alloc_func(cSqlite3Database, allocate);
  rb_define_method(cSqlite3Database, "initialize", initialize, -1);
  rb_define_method(cSqlite3Database, "close", sqlite3_rb_close, 0);
  rb_define_method(cSqlite3Database, "closed?", closed_p, 0);
  rb_define_method(cSqlite3Database, "total_changes", total_changes, 0);
  rb_define_method(cSqlite3Database, "trace", trace, -1);
  rb_define_method(cSqlite3Database, "last_insert_row_id", last_insert_row_id, 0);
  rb_define_method(cSqlite3Database, "define_function", define_function, 1);
  rb_define_method(cSqlite3Database, "define_aggregator", define_aggregator, 2);
  rb_define_method(cSqlite3Database, "interrupt", interrupt, 0);
  rb_define_method(cSqlite3Database, "errmsg", errmsg, 0);
  rb_define_method(cSqlite3Database, "errcode", errcode, 0);
  rb_define_method(cSqlite3Database, "complete?", complete_p, 1);
  rb_define_method(cSqlite3Database, "changes", changes, 0);
  rb_define_method(cSqlite3Database, "authorizer=", set_authorizer, 1);
  rb_define_method(cSqlite3Database, "busy_handler", busy_handler, -1);
  rb_define_method(cSqlite3Database, "busy_timeout=", set_busy_timeout, 1);
  rb_define_method(cSqlite3Database, "copy_to", copy_to, 1);

#ifdef HAVE_RUBY_ENCODING_H
  rb_define_method(cSqlite3Database, "encoding", db_encoding, 0);
#endif

}

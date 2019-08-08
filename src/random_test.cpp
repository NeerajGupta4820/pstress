#include "random_test.hpp"
#include "common.hpp"
#include "node.hpp"

using namespace rapidjson;
using namespace std;
std::mt19937 rng;

const std::string partition_string = "_p";
const int version = 2;
static std::vector<std::string> g_encryption = {"Y", "N"};
static std::vector<std::string> g_row_format;
static std::vector<std::string> g_tablespace;
bool Thd1::connection_lost = false;

static std::vector<int> g_key_block_size;
std::mutex Thd1::ddl_logs_write;

static int g_max_columns_length = 30;
static int g_innodb_page_size = 16;

static std::vector<Table *> *all_tables = new std::vector<Table *>;

/* get result of sql */
static std::string get_result(std::string sql, Thd1 *thd) {
  thd->store_result = true;
  execute_sql(sql, thd);
  auto result = thd->result;
  thd->result = "";
  thd->store_result = false;
  return result;
}

/* return column type from a string */
COLUMN_TYPES col_type(std::string type) {
  if (type.compare("INT") == 0)
    return INT;
  else if (type.compare("CHAR") == 0)
    return CHAR;
  else if (type.compare("VARCHAR") == 0)
    return VARCHAR;
  else if (type.compare("BOOL") == 0)
    return BOOL;
  else
    throw std::runtime_error("unhandled column from string");
}

/* return string from a column type */
std::string col_type(COLUMN_TYPES type) {
  switch (type) {
  case INT:
    return "INT";
  case CHAR:
    return "CHAR";
  case VARCHAR:
    return "VARCHAR";
  case BOOL:
    return "BOOL";
  default:
    throw std::runtime_error("unhandled column from column_types");
  }
}

static std::string db_branch() {
  std::string branch = mysql_get_client_info();
  branch = branch.substr(0, 3); // 8.0 or 5.7
  return branch;
}

/* disable some feature based on user request/ branch/ fork */
int sum_of_all_options() {

  /* if select is set as zero, disable all type of selects */
  if (options->at(Option::SELECT)->getBool() == false) {
    options->at(Option::SELECT_ALL_ROW)->setInt(0);
    options->at(Option::SELECT_ROW_USING_PKEY)->setInt(0);
  }

  /* if delete is set as zero, disable all type of deletes */
  if (options->at(Option::DELETE)->getBool() == false) {
    options->at(Option::DELETE_ALL_ROW)->setInt(0);
    options->at(Option::DELETE_ROW_USING_PKEY)->setInt(0);
  }

  /* If update is disable, set all update probability to zero */
  if (options->at(Option::UPDATE)->getBool() == false) {
    options->at(Option::UPDATE_ROW_USING_PKEY)->setInt(0);
  }

  /* if insert is disable, set all insert probability to zero */
  if (options->at(Option::INSERT)->getBool() == false) {
    opt_int_set(INSERT_RANDOM_ROW, 0);
  }

  /* If no-encryption is set, disable all encryption options */
  auto enc = opt_bool(NO_ENCRYPTION);
  if (enc) {
    opt_int_set(ALTER_TABLE_ENCRYPTION, 0);
    opt_int_set(ALTER_TABLESPACE_ENCRYPTION, 0);
    opt_int_set(ALTER_MASTER_KEY, 0);
  }

  /* for 5.7 disable tablespace rename */
  if (db_branch().compare("5.7") == 0)
    opt_int_set(ALTER_TABLESPACE_RENAME, 0);

  /* for 8.0 default columns */
  if (db_branch().compare("8.0") == 0) {
    auto opt = options->at(Option::COLUMNS);
    if (!opt->cl)
      opt->setInt(10);
  }

  auto only_cl_ddl = opt_bool(ONLY_CL_DDL);
  auto only_cl_sql = opt_bool(ONLY_CL_SQL);
  auto no_ddl = opt_bool(NO_DDL);

  /* only-cl-sql, if set then disable all other DDL */
  if (only_cl_sql) {
    for (auto &opt : *options) {
      if (opt != nullptr && opt->sql && !opt->cl)
        opt->setInt(0);
    }
  }

  /* only-cl-ddl, if set then disable all other DDL */
  if (only_cl_ddl) {
    for (auto &opt : *options) {
      if (opt != nullptr && opt->ddl && !opt->cl)
        opt->setInt(0);
    }
  }

  if (only_cl_ddl && no_ddl)
    throw std::runtime_error("noddl && only-cl-ddl can't be passed together");

  /* if no ddl is set disable all ddl */
  if (no_ddl) {
    for (auto &opt : *options) {
      if (opt != nullptr && opt->sql && opt->ddl)
        opt->setInt(0);
    }
  }

  int total = 0;
  for (auto &opt : *options) {
    if (opt == nullptr || !opt->sql)
      continue;
    std::cout << opt->getName() << " " << opt->getInt() << std::endl;
    total += opt->getInt();
  }
  std::cout << "RETURNING" << std::endl;
  return total;
}

Option::Opt pick_some_option() {
  static int total_probablity = sum_of_all_options();
  int rd = rand_int(total_probablity, 1);
  for (auto &opt : *options) {
    if (opt == nullptr || !opt->sql)
      continue;
    if (rd <= opt->getInt())
      return opt->getOption();
    else
      rd -= opt->getInt();
  }
  return Option::MAX;
}
int sum_of_all_server_options() {
  int total = 0;
  for (auto &opt : *server_options) {
    total += opt->prob;
  }
  return total;
}

/* set seed of current thread */
int set_seed(Thd1 *thd) {
  auto initial_seed = opt_int(INITIAL_SEED);
  rng = std::mt19937(initial_seed);
  thd->thread_log << "Initial seed " << initial_seed;
  for (int i = 0; i < thd->thread_id; i++)
    rand_int(MIN_SEED_SIZE, MAX_SEED_SIZE);
  thd->seed = rand_int(MAX_SEED_SIZE, MIN_SEED_SIZE);
  thd->thread_log << "CURRENT SEED IS " << thd->seed << std::endl;
  return thd->seed;
}

std::vector<std::string> *random_strs_generator(unsigned long int seed) {
  static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz"
                                 "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                 "0123456789";

  static const size_t N_STRS = 10000;

  std::default_random_engine rng(seed);
  std::uniform_int_distribution<> dist(0, sizeof(alphabet) / sizeof(*alphabet) -
                                              2);

  std::vector<std::string> *strs = new std::vector<std::string>;
  strs->reserve(N_STRS);
  std::generate_n(std::back_inserter(*strs), strs->capacity(), [&] {
    std::string str;
    str.reserve(MAX_RANDOM_STRING_SIZE);
    std::generate_n(std::back_inserter(str), MAX_RANDOM_STRING_SIZE,
                    [&]() { return alphabet[dist(rng)]; });

    return str;
  });
  return strs;
}

/* return some random table */
Table *select_random_table() { return all_tables->at(0); }

std::vector<std::string> *random_strs;

int rand_int(int upper, int lower) {
  /*todo change the approach if it is too slow */
  std::uniform_int_distribution<std::mt19937::result_type> dist(
      lower, upper); // distribution in range [lower, upper]
  return dist(rng);
}

/* return random string in range of upper and lower */
std::string rand_string(int upper, int lower) {
  std::string rs = ""; /*random_string*/
  auto size = rand_int(upper, lower);

  /* let the query fail with string size greater */
  if (rand_int(100) < 3)
    size *= 1;

  while (size > 0) {
    auto str = random_strs->at(rand_int(random_strs->size() - 1));
    if (size > MAX_RANDOM_STRING_SIZE)
      rs += str;
    else
      rs += str.substr(0, size);
    size -= MAX_RANDOM_STRING_SIZE;
  }
  return rs;
}

/* return random value of any string */
std::string Column::rand_value() {
  switch (type_) {
  case (COLUMN_TYPES::INT):
    static auto rec = 100 * opt_int(INITIAL_RECORDS_IN_TABLE);
    return std::to_string(rand_int(rec));
    break;
  case (COLUMN_TYPES::VARCHAR):
  case (COLUMN_TYPES::CHAR):
    return "\'" + rand_string(length) + "\'";
    break;
  case (BOOL):
    return (rand_int(1) == 1 ? "true" : "false");
    break;
  default:
    throw std::runtime_error("unhandled column in rand_value " +
                             col_type(type_));
  }
}

/* add new column, part of create table or Alter table */
Column::Column(std::string name, Table *table, int type)
    : name_(name), table_(table) {
  switch (type) {
  case COLUMN_TYPES::CHAR:
    type_ = CHAR;
    length = rand_int(g_max_columns_length, 10);
    break;
  case COLUMN_TYPES::VARCHAR:
    type_ = VARCHAR;
    length = rand_int(g_max_columns_length, 10);
    break;
  case COLUMN_TYPES::INT:
    type_ = INT;
    if (rand_int(10) == 1)
      length = rand_int(100, 20);
    else
      length = 0;
    break;
  case COLUMN_TYPES::BOOL:
    type_ = BOOL;
    break;
  default:
    throw std::runtime_error("unhandled column found in create table");
  }
}

/* used to read metadata */
Column::Column(std::string name, std::string type, bool is_null, int len,
               Table *table)
    : name_(name), null(is_null), length(len), table_(table) {
  type_ = col_type(type);
}

template <typename Writer> void Column::Serialize(Writer &writer) const {
  writer.StartObject();
  writer.String("name");
  writer.String(name_.c_str(), static_cast<SizeType>(name_.length()));
  writer.String("type");
  std::string typ = col_type(type_);
  writer.String(typ.c_str(), static_cast<SizeType>(typ.length()));
  writer.String("null");
  writer.Bool(null);
  writer.String("primary_key");
  writer.Bool(primary_key);
  writer.String("auto_increment");
  writer.Bool(auto_increment);
  writer.String("generated");
  writer.Bool(generated);
  writer.String("lenght");
  writer.Int(length);
  writer.EndObject();
}

template <typename Writer> void Ind_col::Serialize(Writer &writer) const {
  writer.StartObject();
  writer.String("name");
  auto &name = column->name_;
  writer.String(name.c_str(), static_cast<SizeType>(name.length()));
  writer.String("desc");
  writer.Bool(desc);
  writer.String("length");
  writer.Uint(length);
  writer.EndObject();
}

template <typename Writer> void Index::Serialize(Writer &writer) const {
  writer.StartObject();
  writer.String("name");
  writer.String(name_.c_str(), static_cast<SizeType>(name_.length()));
  writer.String(("index_columns"));
  writer.StartArray();
  for (auto ic : *columns_)
    ic->Serialize(writer);
  writer.EndArray();
  writer.EndObject();
}
Index::~Index() {
  for (auto id_col : *columns_) {
    delete id_col;
  }
  delete columns_;
}

template <typename Writer> void Table::Serialize(Writer &writer) const {
  writer.StartObject();

  writer.String("name");
  writer.String(name_.c_str(), static_cast<SizeType>(name_.length()));

  writer.String("engine");
  if (!engine.empty())
    writer.String(engine.c_str(), static_cast<SizeType>(engine.length()));
  else
    writer.String("default");

  writer.String("row_format");
  if (!row_format.empty())
    writer.String(row_format.c_str(),
                  static_cast<SizeType>(row_format.length()));
  else
    writer.String("default");

  writer.String("tablespace");
  if (!tablespace.empty())
    writer.String(tablespace.c_str(),
                  static_cast<SizeType>(tablespace.length()));
  else
    writer.String("file_per_table");

  writer.String("has_pk");
  writer.Bool(has_pk);

  if (has_pk) {
    writer.String("max_pk_value_inserted");
    writer.Int(max_pk_value_inserted);
  }

  writer.String("encryption");
  writer.Bool(encryption);

  writer.String("key_block_size");
  writer.Int(key_block_size);

  writer.String(("columns"));
  writer.StartArray();
  for (auto &col : *columns_)
    col->Serialize(writer);
  writer.EndArray();

  writer.String(("indexes"));
  writer.StartArray();
  for (auto *ind : *indexes_)
    ind->Serialize(writer);
  writer.EndArray();
  writer.EndObject();
}

Ind_col::Ind_col(Column *c) : column(c) {}

Ind_col::Ind_col(Column *c, bool d) : column(c), desc(d) {}

Index::Index(std::string n) : name_(n), columns_() {
  columns_ = new std::vector<Ind_col *>;
}

void Index::AddInternalColumn(Ind_col *column) { columns_->push_back(column); }

Table::Table(std::string n) : name_(n), max_pk_value_inserted(0), indexes_() {
  columns_ = new std::vector<Column *>;
  indexes_ = new std::vector<Index *>;
}

void Table::DropCreate(Thd1 *thd) {
  if (execute_sql("DROP TABLE " + name_, thd))
    max_pk_value_inserted = 0;

  std::string def = Table_defination();
  if (execute_sql(def, thd))
    max_pk_value_inserted = 0;
  else if (tablespace.size() > 0) {
    std::string tbs = " TABLESPACE=" + tablespace + "_rename";

    auto no_encryption = opt_bool(NO_ENCRYPTION);

    std::string encrypt_sql = " ENCRYPTION = ";
    encrypt_sql += (encryption == false ? "'y' " : "'n'");

    /* If tablespace is rename */
    if (execute_sql(def + tbs, thd))
      max_pk_value_inserted = 0;

    /* If tablespace is encrypted, and or tablespace is rename */
    else if (!no_encryption && (execute_sql(def + encrypt_sql, thd) ||
                                execute_sql(def + encrypt_sql + tbs, thd))) {
      table_mutex.lock();
      encryption = !encryption;
      table_mutex.unlock();
      max_pk_value_inserted = 0;
    }
  }
}

void Table::Optimize(Thd1 *thd) { execute_sql("OPTIMIZE TABLE " + name_, thd); }

void Table::Analyze(Thd1 *thd) { execute_sql("ANALYZE TABLE " + name_, thd); }

void Table::Truncate(Thd1 *thd) {
  if (execute_sql("TRUNCATE TABLE " + name_, thd))
    max_pk_value_inserted = 0;
}

Table::~Table() {
  for (auto ind : *indexes_)
    delete ind;
  for (auto col : *columns_) {
    delete col;
  }
  delete columns_;
  delete indexes_;
}

/* create default column */
void Table::CreateDefaultColumn() {
  auto no_auto_inc = opt_bool(NO_AUTO_INC);

  /* create normal column */
  static auto max_col = opt_int(COLUMNS);
  auto max_columns = rand_int(max_col, 1);
  bool auto_increment = false;
  for (int i = 0; i < max_columns; i++) {
    std::string name;
    COLUMN_TYPES type;
    Column *col;
    /*  if we need to create primary column */
    static int pkey_pb_per_table = opt_int(PRIMARY_KEY);

    /* First column can be primary */
    if (i == 0 && rand_int(100) < pkey_pb_per_table) {
      type = COLUMN_TYPES::INT;
      name = "pkey";
      has_pk = true;
      col = new Column{name, this, type};
      col->primary_key = true;
      if (!no_auto_inc &&
          rand_int(3) < 3) { /* 75% of primary key tables are autoinc */
        col->auto_increment = true;
        auto_increment = true;
      }

    } else {
      name = "c" + std::to_string(i);
      auto t = rand_int(COLUMN_TYPES::COLUMN_MAX - 1);
      col = new Column{name, this, t};
      /* 25% column can have auto_inc */
      if (col->type_ == INT && !no_auto_inc && auto_increment == false &&
          rand_int(100) > 25) {
        col->auto_increment = true;
        auto_increment = true;
      }
    }
    AddInternalColumn(col);
  }

  /* create some generated columns */
  // std::string name = "g" + to_string(1);
}

/* create default indexes */
void Table::CreateDefaultIndex() {
  int auto_inc_col = -1;

  /* number of index we are adding */
  int max_indexes = opt_int(INDEXES);
  if (max_indexes > (int)columns_->size())
    max_indexes = columns_->size();
  int indexes = rand_int(max_indexes, 1);

  /* for auto-inc columns handling, we need to  add auto_inc  as first column */
  for (size_t i = 0; i < columns_->size(); i++) {
    if (columns_->at(i)->auto_increment) {
      auto_inc_col = i;
    }
  }
  int auto_inc_index = rand_int(indexes, 1);

  for (int i = 0; i < indexes; i++) {
    Index *id = new Index(name_ + "i" + std::to_string(i));

    /* number of columns we are adding in index */
    size_t max_columns = opt_int(INDEX_COLUMNS);
    if (max_columns > columns_->size())
      max_columns = columns_->size();
    int columns = rand_int(max_columns, 1);

    std::vector<int> col_pos; // position of columns

    while (col_pos.size() < (size_t)columns) {
      int current = rand_int(columns - 1);
      bool flag = false;

      /* auto-inc column should be part of  */
      if (auto_inc_col != -1 && auto_inc_index == i + 1 &&
          col_pos.size() == 0) {
        col_pos.push_back(auto_inc_col);
        continue;
      }

      for (auto id : col_pos) {
        if (id == current) {
          flag = true;
          break;
        }
      }
      if (flag == false)
        col_pos.push_back(current);
    }

    for (auto pos : col_pos) {
      auto col = columns_->at(pos);
      static bool no_desc_support = opt_bool(NO_DESC_INDEX);
      bool column_desc = false;
      if (!no_desc_support) {
        column_desc = rand_int(100) < DESC_INDEXES_IN_COLUMN
                          ? true
                          : false; // 33 % are desc //
      }
      id->AddInternalColumn(
          new Ind_col(col, column_desc)); // desc is set as true
    }
    AddInternalIndex(id);
  }
}

/* Create new table and pick some attributes */
Table *Table::table_id(TABLE_TYPES type, int id, Thd1 *thd) {
  Table *table;
  std::string name = "tt_" + std::to_string(id);
  switch (type) {
  case PARTITION:
    table = new Partition_table(name + partition_string);
    break;
  case NORMAL:
    table = new Table(name);
    break;
  case TEMPORARY:
    table = new Temporary_table(name + "_t");
    break;
  default:
    throw std::runtime_error("Unhandle Table type");
    break;
  }

  table->type = type;

  static auto no_encryption = opt_bool(NO_ENCRYPTION);
  static auto branch = db_branch();

  if (table->type != TEMPORARY && !no_encryption && g_encryption.size() > 0 &&
      g_encryption[rand_int(g_encryption.size() - 1)].compare("Y") == 0)
    table->encryption = true;

  /* temporary table on 8.0 can't have key block size */
  if (!(branch.compare("8.0") == 0 && type == TEMPORARY)) {
    if (g_key_block_size.size() > 0)
      table->key_block_size =
          g_key_block_size[rand_int(g_key_block_size.size() - 1)];

    if (table->key_block_size > 0 && rand_int(2) == 0) {
      table->row_format = "COMPRESSED";
    }

    if (table->key_block_size == 0 && g_row_format.size() > 0)
      table->row_format = g_row_format[rand_int(g_row_format.size() - 1)];
  }

  /* with more number of tablespace there are more chances to have table in
   * tablespaces */
  static int tbs_count = opt_int(NUMBER_OF_GENERAL_TABLESPACE);

  /* temporary table can't have tablespace */
  if (table->type != TEMPORARY && g_tablespace.size() > 0 &&
      rand_int(tbs_count) != 0) {
    table->tablespace = g_tablespace[rand_int(g_tablespace.size() - 1)];
    table->encryption =
        false; // todo add encrypted tablespace in starting if supported //
    table->row_format.clear();
    if (g_innodb_page_size > INNODB_16K_PAGE_SIZE ||
        table->tablespace.compare("innodb_system") == 0 ||
        stoi(table->tablespace.substr(3, 2)) == g_innodb_page_size)
      table->key_block_size = 0;
    else
      table->key_block_size = std::stoi(table->tablespace.substr(3, 2));
  }

  /* if temporary table encrypt variable set create encrypt table */
  static auto temp_table_encrypt =
      get_result("select @@innodb_temp_tablespace_encrypt", thd);

  // todo use #ifdefine FORK
  if (strcmp(FORK, "Percona-Server") == 0 && table->type == TEMPORARY &&
      temp_table_encrypt.compare("1") == 0)
    table->encryption = true;

  /* if innodb system is encrypt , create ecrypt table */
  static auto system_table_encrypt =
      get_result("select @@innodb_sys_tablespace_encrypt", thd);

  if (strcmp(FORK, "Percona-Server") == 0 && table->tablespace.size() > 0 &&
      table->tablespace.compare("innodb_system") == 0 &&
      system_table_encrypt.compare("1") == 0) {
    table->encryption = true;
  }

  static auto engine = options->at(Option::ENGINE)->getString();
  table->engine = engine;

  table->CreateDefaultColumn();
  table->CreateDefaultIndex();

  return table;
}

/* prepare table defination */
std::string Table::Table_defination() {
  std::string def = "CREATE";
  if (type == TABLE_TYPES::TEMPORARY)
    def += " TEMPORARY";
  def += " TABLE  " + name_ + " (";

  // todo move to method
  for (auto col : *columns_) {
    def += " " + col->name_ + " " + col_type(col->type_);
    if (col->length > 0)
      def += "(" + std::to_string(col->length) + ")";
    if (col->null)
      def += " NOT NULL";
    if (col->auto_increment)
      def += " AUTO_INCREMENT";
    def += ", ";
  }

  /* add primary key */
  if (has_pk) {
    def += " PRiMARY KEY(";
    for (auto col : *columns_) {
      if (col->primary_key)
        def += col->name_ + ", ";
    }
    def.erase(def.length() - 2);
    def += "), ";
  }

  for (auto id : *indexes_) {
    def += "INDEX " + id->name_ + "(";
    for (auto idc : *id->columns_) {
      def += idc->column->name_;
      def += (idc->desc ? " DESC" : (rand_int(3) ? "" : " ASC"));
      def += ", ";
    }
    def.erase(def.length() - 2);
    def += "), ";
  }
  def.erase(def.length() - 2);
  def += " )";

  if (encryption)
    def += " ENCRYPTION='Y'";
  else if (type != TEMPORARY && rand_int(3) == 1)
    def += " ENCRYPTION='N'";

  if (!tablespace.empty())
    def += " TABLESPACE=" + tablespace;

  if (key_block_size > 0)
    def += " KEY_BLOCK_SIZE=" + std::to_string(key_block_size);

  if (row_format.size() > 0)
    def += " ROW_FORMAT=" + row_format;

  if (!engine.empty())
    def += " ENGINE=" + engine;

  return def;
}

/* create default table include all tables now */
void create_default_tables(Thd1 *thd) {
  auto tables = opt_int(TABLES);
  auto only_temporary_tables = opt_bool(ONLY_TEMPORARY);
  if (!only_temporary_tables) {
    for (int i = 1; i <= tables; i++) {
      all_tables->push_back(Table::table_id(TABLE_TYPES::NORMAL, i, thd));
      all_tables->push_back(Table::table_id(TABLE_TYPES::PARTITION, i, thd));
    }
  }
}

/* return true if SQL is successful, else return false */
bool execute_sql(std::string sql, Thd1 *thd) {
  auto query = sql.c_str();
  auto res = mysql_real_query(thd->conn, query, strlen(query));
  static auto log_all = opt_bool(LOG_ALL_QUERIES);
  static auto log_failed = opt_bool(LOG_FAILED_QUERIES);
  static auto log_success = opt_bool(LOG_SUCCEDED_QUERIES);
  sql += ";";

  if (res == 1) {
    /* log failed query */
    if (log_all || log_failed) {
      thd->thread_log << "Query =>" << sql << std::endl;
      thd->thread_log << "Error " << mysql_error(thd->conn) << std::endl;
    }
    if (mysql_errno(thd->conn) == 2006) {
      thd->thread_log << "server gone, while processing " + sql;
      thd->connection_lost = true;
    }
  } else {
    MYSQL_RES *result;
    result = mysql_store_result(thd->conn);

    /* log result */
    if (thd->store_result) {
      if (!result)
        throw std::runtime_error(sql + " doest not return result set");
      auto row = mysql_fetch_row(result);
      thd->result = row[0];
    }

    /* log successful query */
    if (log_all || log_success) {
      thd->thread_log << "Query  =>" << sql << std::endl;
      if (!result) {
        thd->thread_log << std::endl;
      } else {
        auto no = mysql_num_rows(result);
        thd->thread_log << " rows:" << no << std::endl;
      }
    }
    mysql_free_result(result);
  }

  if (thd->ddl_query) {
    thd->ddl_logs_write.lock();
    thd->ddl_logs << thd->thread_id << " " << sql << " "
                  << mysql_error(thd->conn) << std::endl;
    thd->ddl_logs_write.unlock();
  }
  return (res == 0 ? 1 : 0);
}

/* load some records in table */
void load_default_data(Table *table, Thd1 *thd) {
  static int initial_records =
      options->at(Option::INITIAL_RECORDS_IN_TABLE)->getInt();
  int rec = rand_int(initial_records);
  for (int i = 0; i < rec; i++) {
    table->InsertRandomRow(thd, false);
  }
}

void Table::SetEncryption(Thd1 *thd) {
  std::string sql = "ALTER TABLE " + name_ + " ENCRYPTION = '";
  std::string enc = g_encryption[rand_int(g_encryption.size() - 1)];
  sql += enc + "'";
  if (execute_sql(sql, thd)) {
    table_mutex.lock();
    if (enc.compare("Y"))
      encryption = true;
    else
      encryption = false;
    table_mutex.unlock();
  }
}

/* alter table add random column */
void Table::AddColumn(Thd1 *thd) {
  std::string sql = "ALTER TABLE " + name_ + "  ADD COLUMN ";
  std::string name;
  name = "COL" + std::to_string(rand_int(300));
  /* choose which column to add */
  auto type = rand_int(COLUMN_TYPES::COLUMN_MAX - 1);

  Column *tc = new Column(name, this, type);
  sql += name + " " + col_type(tc->type_);
  if (tc->length > 0)
    sql += "(" + std::to_string(tc->length) + ")";

  if (execute_sql(sql, thd)) {
    table_mutex.lock();
    AddInternalColumn(tc);
    table_mutex.unlock();
  } else
    delete tc;
}

void Table::DeleteAllRows(Thd1 *thd) {
  std::string sql = "DELETE FROM " + name_;
  execute_sql(sql, thd);
}

void Table::SelectAllRow(Thd1 *thd) {
  std::string sql = "SELECT * FROM " + name_;
  execute_sql(sql, thd);
}

void Table::ColumnRename(Thd1 *thd) {
  table_mutex.lock();
  auto ps = rand_int(columns_->size() - 1);
  auto name = columns_->at(ps)->name_;
  /* ALTER column to _rename or back to orignal_name */
  std::string new_name = "_rename";
  static auto s = new_name.size();
  if (name.size() > s && name.substr(name.length() - s).compare("_rename") == 0)
    new_name = name.substr(0, name.length() - s);
  else
    new_name = name + new_name;
  std::string sql =
      "ALTER TABLE " + name_ + " RENAME COLUMN " + name + " To " + new_name;
  table_mutex.unlock();
  if (execute_sql(sql, thd)) {
    table_mutex.lock();
    for (auto &col : *columns_) {
      if (col->name_.compare(name) == 0)
        col->name_ = new_name;
    }
    table_mutex.unlock();
  }
}

/* pick random columns for delete */
inline int Table::pick_column_for_delete() {
  auto column = rand_int(columns_->size() - 1);

  bool only_bool = true;
  /* if tables has pkey try to use that in where clause */
  if (has_pk && rand_int(100) <= 50)
    return 0;

  for (auto &col : *columns_) {
    if (col->type_ != BOOL) {
      only_bool = false;
      break;
    }
  }

  if (columns_->at(column)->type_ == BOOL && rand_int(100) == 1)
    return column;

  if (columns_->at(column)->type_ == BOOL && only_bool == false)
    return pick_column_for_delete();

  return column;
}

void Table::DeleteRandomRow(Thd1 *thd) {
  table_mutex.lock();
  auto where = pick_column_for_delete();

  std::string sql = "DELETE FROM " + name_ + " WHERE " +
                    columns_->at(where)->name_ + "=" +
                    columns_->at(where)->rand_value();
  table_mutex.unlock();
  execute_sql(sql, thd);
}

void Table::SelectRandomRow(Thd1 *thd) {
  table_mutex.lock();
  auto where = rand_int(columns_->size() - 1);
  std::string sql = "SELECT * FROM " + name_ + " WHERE " +
                    columns_->at(where)->name_ + "=" +
                    columns_->at(where)->rand_value();
  table_mutex.unlock();
  execute_sql(sql, thd);
}
void Table::UpdateRandomROW(Thd1 *thd) {
  table_mutex.lock();
  auto set = rand_int(columns_->size() - 1);
  auto where = rand_int(columns_->size() - 1);

  /* if tables has pkey try to use that in where clause */
  if (has_pk && rand_int(100) <= 50)
    where = 0;

  std::string sql = "UPDATE " + name_ + " SET " + columns_->at(set)->name_ +
                    "=" + columns_->at(set)->rand_value() + " WHERE " +
                    columns_->at(where)->name_ + "=" +
                    columns_->at(where)->rand_value();
  table_mutex.unlock();
  execute_sql(sql, thd);
}

void Table::InsertRandomRow(Thd1 *thd, bool is_lock) {
  if (is_lock)
    table_mutex.lock();
  std::string vals = "";
  std::string insert = "INSERT INTO " + name_ + "  ( ";
  for (auto &column : *columns_) {
    insert += column->name_ + " ,";
    auto val = column->rand_value();
    if (column->auto_increment == true && rand_int(100) < 10)
      val = "NULL";
    vals += " " + val + ",";
  }

  if (vals.size() > 0) {
    vals.pop_back();
    insert.pop_back();
  }
  insert += ") VALUES(" + vals;
  insert += " )";
  if (is_lock)
    table_mutex.unlock();
  if (execute_sql(insert, thd))
    max_pk_value_inserted++;
}

/* alter table drop column */
void Table::DropColumn(Thd1 *thd) {
  table_mutex.lock();
  if (columns_->size() == 1) {
    table_mutex.unlock();
    return;
  }
  auto ps = rand_int(columns_->size() - 1);
  auto name = columns_->at(ps)->name_;
  if (name.compare("pkey") == 0 || name.compare("pkey_rename") == 0) {
    table_mutex.unlock();
    return;
  }
  std::string sql = "ALTER TABLE " + name_ + " DROP COLUMN " + name;
  table_mutex.unlock();

  if (execute_sql(sql, thd)) {
    table_mutex.lock();

    std::vector<int> indexes_to_drop;
    for (auto id = indexes_->begin(); id != indexes_->end(); id++) {
      auto index = *id;

      for (auto id_col = index->columns_->begin();
           id_col != index->columns_->end(); id_col++) {
        auto ic = *id_col;
        if (ic->column->name_.compare(name) == 0) {
          if (index->columns_->size() == 1) {
            delete index;
            indexes_to_drop.push_back(id - indexes_->begin());
          } else {
            delete ic;
            index->columns_->erase(id_col);
          }
          break;
        }
      }
    }
    std::sort(indexes_to_drop.begin(), indexes_to_drop.end(),
              std::greater<int>());

    for (auto &i : indexes_to_drop) {
      indexes_->at(i) = indexes_->back();
      indexes_->pop_back();
    }
    // table->indexes_->erase(id);

    for (auto pos = columns_->begin(); pos != columns_->end(); pos++) {
      if ((*pos)->name_.compare(name) == 0) {
        delete *pos;
        columns_->erase(pos);
        break;
      }
    }
    table_mutex.unlock();
  }
}

/* set mysqld_variable */
void set_mysqld_variable(Thd1 *thd) {
  static int total_probablity = sum_of_all_server_options();
  int rd = rand_int(total_probablity);
  for (auto &opt : *server_options) {
    if (rd <= opt->prob) {
      std::string sql = "SET ";
      sql += rand_int(3) == 0 ? " SESSION " : " GLOBAL ";
      sql += opt->name + "=" + opt->values.at(rand_int(opt->values.size() - 1));
      execute_sql(sql, thd);
    }
  }
}

/* alter tablespace set encryption */
void alter_tablespace_encryption(Thd1 *thd) {
  if (g_tablespace.size() > 0) {
    std::string sql = "ALTER TABLESPACE " +
                      g_tablespace[rand_int(g_tablespace.size() - 1)] +
                      " ENCRYPTION ";
    sql += (rand_int(1) == 0 ? "'Y'" : "'N'");
    execute_sql(sql, thd);
  }
}

/* alter tablespace rename */
void alter_tablespace_rename(Thd1 *thd) {
  if (g_tablespace.size() > 0) {
    auto tablespace = g_tablespace[rand_int(g_tablespace.size() - 1),
                                   1]; // don't pick innodb_system;
    std::string sql = "ALTER tablespace " + tablespace;
    if (rand_int(1) == 0)
      sql += "_rename  RENAME TO " + tablespace;
    else
      sql += " RENAME TO " + tablespace + "_rename";
    execute_sql(sql, thd);
  }
}

/* save objects to a file */
void save_objects_to_file() {
  //  rapidjson::Writer<Stream> writer(stream);

  StringBuffer sb;
  PrettyWriter<StringBuffer> writer(sb);
  writer.StartObject();
  writer.String("version");
  writer.Uint(version);

  writer.String(("tables"));
  writer.StartArray();
  for (auto j = all_tables->begin(); j != all_tables->end(); j++) {
    auto table = *j;
    table->Serialize(writer);
  }
  writer.EndArray();
  writer.EndObject();
  auto file = opt_string(METADATA_WRITE_FILE);
  std::ofstream of(file);
  of << sb.GetString();
  if (!of.good())
    throw std::runtime_error("can't write the JSON string to the file!");
}

/*load objects from a file */
void load_objects_from_file(Thd1 *thd) {
  std::string file_read_path = opt_string(LOGDIR);
  file_read_path += "/" + opt_string(METADATA_READ_FILE);
  thd->thread_log << "reading metadata from file " << file_read_path
                  << std::endl;
  FILE *fp = fopen(file_read_path.c_str(), "r");
  char readBuffer[65536];
  FileReadStream is(fp, readBuffer, sizeof(readBuffer));
  Document d;
  d.ParseStream(is);
  auto v = d["version"].GetInt();

  if (d["version"].GetInt() != version)
    throw std::runtime_error("version mismatch between " + file_read_path +
                             " and codebase " + " file::version is " +
                             std::to_string(v) + " code::version is " +
                             std::to_string(version));

  for (auto &tab : d["tables"].GetArray()) {
    Table *table;
    std::string name = tab["name"].GetString();

    std::string table_type = name.substr(name.size() - 2, 2);

    if (table_type.compare(partition_string) == 0)
      table = new Partition_table(name);
    else
      table = new Table(name);

    std::string engine = tab["engine"].GetString();
    if (engine.compare("default") != 0) {
      table->engine = engine;
    }

    std::string row_format = tab["row_format"].GetString();
    if (row_format.compare("default") != 0) {
      table->row_format = row_format;
    }

    std::string tablespace = tab["tablespace"].GetString();
    if (tablespace.compare("file_per_table") != 0) {
      table->tablespace = tablespace;
    }

    bool has_pk = tab["has_pk"].GetBool();
    table->has_pk = has_pk;

    if (has_pk)
      table->max_pk_value_inserted = tab["max_pk_value_inserted"].GetInt();

    table->encryption = tab["encryption"].GetBool();

    table->key_block_size = tab["key_block_size"].GetInt();

    for (auto &col : tab["columns"].GetArray()) {
      Column *a =
          new Column(col["name"].GetString(), col["type"].GetString(),
                     col["null"].GetBool(), col["lenght"].GetInt(), table);
      a->auto_increment = col["auto_increment"].GetBool();
      a->generated = col["generated"].GetBool();
      a->primary_key = col["primary_key"].GetBool();
      table->AddInternalColumn(a);
    }

    for (auto &ind : tab["indexes"].GetArray()) {
      Index *index = new Index(ind["name"].GetString());

      for (auto &ind_col : ind["index_columns"].GetArray()) {
        std::string index_base_column = ind_col["name"].GetString();

        for (auto &column : *table->columns_) {
          if (index_base_column.compare(column->name_) == 0) {
            index->AddInternalColumn(
                new Ind_col(column, ind_col["desc"].GetBool()));
            break;
          }
        }
      }
      table->AddInternalIndex(index);
    }

    all_tables->push_back(table);
    options->at(Option::TABLES)->setInt(all_tables->size());
  }
  fclose(fp);
}

/* clean tables from memory,random_strs */
void clean_up_at_end() {
  for (auto &table : *all_tables)
    delete table;
  delete all_tables;
  delete random_strs;
}

/* create new database and tables */
void create_database_tablespace(Thd1 *thd) {

  auto no_tbs = opt_bool(NO_TABLESPACE);
  if (!no_tbs)
    g_tablespace = {"innodb_system", "tab02k", "tab04k", "tab01k"};

  std::string row_format = opt_string(ROW_FORMAT);

  if (row_format.compare("uncompressed") == 0) {
    g_row_format = {"DYNAMIC", "REDUNDANT"};
  } else if (row_format.compare("all") == 0) {
    g_row_format = {"DYNAMIC", "REDUNDANT", "COMPRESSED"};
    g_key_block_size = {0, 0, 1, 2, 4};

  } else if (row_format.compare("none") == 0) {
    thd->thread_log << "row_format is excluted" << std::endl;
  } else {
    g_row_format.push_back(row_format);
  }

  if (g_innodb_page_size > INNODB_16K_PAGE_SIZE) {
    g_row_format.clear();
    g_key_block_size.clear();
  }
  if (!no_tbs) {
    /* Adjust the tablespaces */
    if (g_innodb_page_size >= INNODB_8K_PAGE_SIZE) {
      g_tablespace.push_back("tab08k");
    }
    if (g_innodb_page_size >= INNODB_16K_PAGE_SIZE) {
      g_tablespace.push_back("tab16k");
    }
    if (g_innodb_page_size >= INNODB_32K_PAGE_SIZE) {
      g_tablespace.push_back("tab32k");
    }
    if (g_innodb_page_size >= INNODB_64K_PAGE_SIZE) {
      g_tablespace.push_back("tab64k");
    }
  }

  auto load_from_file = opt_bool(METADATA_READ);

  /* drop dabase test*/
  if (!load_from_file) {
    execute_sql("DROP DATABASE test", thd);
    execute_sql("CREATE DATABASE test", thd);
  }

  /* add addtional tablespace */
  auto tbs_count = opt_int(NUMBER_OF_GENERAL_TABLESPACE);
  if (tbs_count > 1) {
    auto current_size = g_tablespace.size();
    for (size_t i = 0; i < current_size; i++) {
      if (g_tablespace[i].compare("innodb_system") == 0)
        continue;
      for (int j = 1; j <= tbs_count; j++)
        g_tablespace.push_back(g_tablespace[i] + to_string(j));
    }
  }

  for (auto &tab : g_tablespace) {
    if (tab.compare("innodb_system") == 0)
      continue;
    std::string sql =
        "CREATE TABLESPACE " + tab + " ADD DATAFILE '" + tab + ".ibd' ";
    if (g_innodb_page_size <= INNODB_16K_PAGE_SIZE) {
      sql += " FILE_BLOCK_SIZE " + tab.substr(3, 3);
    }
    if (!load_from_file) {
      if (db_branch().compare("5.7") != 0)
        execute_sql("ALTER TABLESPACE " + tab + "_rename rename to " + tab,
                    thd);
      execute_sql("DROP TABLESPACE " + tab, thd);
      execute_sql(sql, thd);
    }
  }
  thd->thread_log << "default tablespaces created" << std::endl;
}

/* load metadata */
bool load_metadata(Thd1 *thd) {
  auto engine = opt_string(ENGINE);

  random_strs =
      random_strs_generator(options->at(Option::INITIAL_SEED)->getInt());
  auto &logs = thd->thread_log;

  /*set seed for current thread*/
  auto initial_seed = opt_int(INITIAL_SEED);
  rng = std::mt19937(initial_seed);
  logs << " creating tables metadata " << std::endl;

  auto load_from_file = opt_bool(METADATA_READ);

  if (load_from_file) {
    load_objects_from_file(thd);
  } else {
    thd->ddl_query = true;
    create_database_tablespace(thd);
    create_default_tables(thd);
  }

  if (options->at(Option::TABLES)->getInt() <= 0)
    throw std::runtime_error("no table to work on \n");

  return 1;
}

void run_some_query(Thd1 *thd, std::atomic<int> &threads_create_table) {

  auto database = opt_string(DATABASE);
  execute_sql("USE " + database, thd);

  thd->ddl_query = true;
  static bool just_ddl = opt_bool(JUST_LOAD_DDL);
  auto size = all_tables->size();

  auto load_from_file = opt_bool(METADATA_READ);
  int threads = opt_int(THREADS);

  if (!load_from_file) {
    /* create tables in all threads */
    for (size_t i = thd->thread_id; i < size; i = i + threads) {
      auto table = all_tables->at(i);
      thd->ddl_query = true;
      if (!execute_sql(table->Table_defination(), thd))
        throw std::runtime_error("Create table failed " + table->name_);
      static auto load_from_file = opt_bool(METADATA_READ);
      if (!just_ddl && !load_from_file) {
        thd->ddl_query = false;
        load_default_data(table, thd);
      }
    }
  }

  /* create session temporary tables */
  auto no_of_tables = opt_int(TABLES);
  no_of_tables /= threads * 2;
  thd->thread_log << "Creating " << no_of_tables << " temp tables "
                  << std::endl;

  std::vector<Table *> *all_temp_tables = new std::vector<Table *>;
  for (int i = 0; i < no_of_tables; i++) {
    thd->ddl_query = true;
    Table *table = Table::table_id(TEMPORARY, i, thd);
    if (!execute_sql(table->Table_defination(), thd))
      throw std::runtime_error("Create table failed " + table->name_);
    all_temp_tables->push_back(table);
    if (!just_ddl) {
      thd->ddl_query = false;
      load_default_data(table, thd);
    }
  }

  threads_create_table++;
  std::atomic<int> initial_threads(threads);
  while (initial_threads != threads_create_table) {
    thd->thread_log << "Waiting for all threds to finish initial load "
                    << std::endl;
    std::chrono::seconds dura(3);
    std::this_thread::sleep_for(dura);
  }

  if (just_ddl)
    return;

  auto sec = opt_int(NUMBER_OF_SECONDS_WORKLOAD);
  auto begin = std::chrono::system_clock::now();
  auto end =
      std::chrono::system_clock::time_point(begin + std::chrono::seconds(sec));

  /* set seed for current thread */
  rng = std::mt19937(set_seed(thd));
  thd->thread_log << thd->thread_id << " value of rand_int(100) "
                  << rand_int(100) << std::endl;

  int total_size = size + no_of_tables;
  /* combine all_tables with temp_tables */

  while (std::chrono::system_clock::now() < end) {

    auto curr = rand_int(total_size - 1);

    auto table = (curr < no_of_tables) ? all_temp_tables->at(curr)
                                       : all_tables->at(curr - no_of_tables);

    auto option = pick_some_option();

    thd->ddl_query = options->at(option)->ddl == true ? true : false;

    switch (option) {
    case Option::DROP_COLUMN:
      table->DropColumn(thd);
      break;
    case Option::TRUNCATE:
      table->Truncate(thd);
      break;
    case Option::ADD_COLUMN:
      table->AddColumn(thd);
      break;
    case Option::DROP_CREATE:
      table->DropCreate(thd);
      break;
    case Option::ALTER_TABLE_ENCRYPTION:
      table->SetEncryption(thd);
      break;
    case Option::SET_GLOBAL_VARIABLE:
      set_mysqld_variable(thd);
      break;
    case Option::ALTER_TABLESPACE_ENCRYPTION:
      alter_tablespace_encryption(thd);
      break;
    case Option::ALTER_TABLESPACE_RENAME:
      alter_tablespace_rename(thd);
      break;
    case Option::SELECT_ALL_ROW:
      table->SelectAllRow(thd);
      break;
    case Option::SELECT_ROW_USING_PKEY:
      table->SelectRandomRow(thd);
      break;
    case Option::INSERT_RANDOM_ROW:
      table->InsertRandomRow(thd, true);
      break;
    case Option::DELETE_ALL_ROW:
      table->DeleteAllRows(thd);
      break;
    case Option::DELETE_ROW_USING_PKEY:
      table->DeleteRandomRow(thd);
      break;
    case Option::UPDATE_ROW_USING_PKEY:
      table->UpdateRandomROW(thd);
      break;
    case Option::OPTIMIZE:
      table->Optimize(thd);
      break;
    case Option::ANALYZE:
      table->Analyze(thd);
      break;
    case Option::RENAME_COLUMN:
      table->ColumnRename(thd);
      break;
    case Option::ALTER_MASTER_KEY:
      execute_sql("ALTER INSTANCE ROTATE INNODB MASTER KEY", thd);
      break;
    default:
      throw std::runtime_error("invalid options");
    }

    if (thd->connection_lost) {
      break;
    }
  }
  for (auto &table : *all_temp_tables)
    delete table;
  delete all_temp_tables;
}


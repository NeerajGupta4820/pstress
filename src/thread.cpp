#include "common.hpp"
#include "node.hpp"
#include "random_test.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>
#include <sstream>
std::atomic_flag lock_metadata = ATOMIC_FLAG_INIT;
std::atomic<bool> metadata_loaded(false);

// Mutex for thread-safe logging
std::mutex general_log_mutex;
// Get the number of affected rows safely
inline unsigned long long Node::getAffectedRows(MYSQL *connection) {
  if (mysql_affected_rows(connection) == ~(unsigned long long)0) {
    return 0LL;
  }
  return mysql_affected_rows(connection);
}

void Node::workerThread(int number) {

  std::ofstream thread_log;
  std::ofstream client_log;
  if (options->at(Option::LOG_CLIENT_OUTPUT)->getBool()) {
    std::ostringstream cl;
    cl << myParams.logdir << "/" << myParams.myName << "_step_"
       << std::to_string(options->at(Option::STEP)->getInt()) << "_thread-"
       << number << ".out";
    client_log.open(cl.str(), std::ios::out | std::ios::trunc);
    if (!client_log.is_open()) {
      general_log << "Unable to open logfile for client output " << cl.str()
                  << ": " << std::strerror(errno) << std::endl;
      return;
    }
  }
  bool log_all_queries = options->at(Option::LOG_ALL_QUERIES)->getBool();
  size_t n_queries = 5;
  if(options->at(Option::LOG_N_QUERIES) && options->at(Option::LOG_N_QUERIES)->getInt() >= 0) {
      n_queries = options->at(Option::LOG_N_QUERIES)->getInt();
  }

 // Prepare log filename based on logging mode
  std::ostringstream log_filename;
  log_filename<< myParams.logdir << "/" << myParams.myName << "_step_"
              << std::to_string(options->at(Option::STEP)->getInt()) << "_thread-"
              << number;
  // Construct full log filename
  std::string full_log_filename = log_all_queries 
        ? (log_filename.str()  + ".sql")  // All queries log
        : (log_filename.str() + "_Last" 
          + "-" + std::to_string(n_queries) +"_queries" ".sql");
  
  // Thread log file setup
  thread_log.open(full_log_filename, std::ios::out | std::ios::trunc);

  if (!thread_log.is_open()) {
      general_log << "Unable to open thread logfile " << full_log_filename 
                  << ": " << std::strerror(errno) << std::endl;
      return;
  }

  if (options->at(Option::LOG_QUERY_DURATION)->getBool()) {
    thread_log.precision(3);
    thread_log << std::fixed;
    std::cerr.precision(3);
    std::cerr << std::fixed;
    std::cout.precision(3);
    std::cout << std::fixed;
  }

  MYSQL *conn;

  conn = mysql_init(NULL);
  if (conn == NULL) {
    thread_log << "Error " << mysql_errno(conn) << ": " << mysql_error(conn)
               << std::endl;

    if (thread_log) {
      thread_log.close();
    }
    general_log << ": Thread #" << number << " is exiting abnormally"
                << std::endl;
    return;
  }
  /*
#ifdef MAXPACKET
  if (myParams.maxpacket != MAX_PACKET_DEFAULT) {
    mysql_options(conn, MYSQL_OPT_MAX_ALLOWED_PACKET, &myParams.maxpacket);
  }
#endif
*/
  if (mysql_real_connect(conn, myParams.address.c_str(),
                         myParams.username.c_str(), myParams.password.c_str(),
                         myParams.database.c_str(), myParams.port,
                         myParams.socket.c_str(), 0) == NULL) {
    thread_log << "Error " << mysql_errno(conn) << ": " << mysql_error(conn)
               << std::endl;
    mysql_close(conn);

    if (thread_log.is_open()) {
      thread_log.close();
    }
    mysql_thread_end();
    return;
  }

  Thd1 *thd = new Thd1(number, thread_log, general_log, client_log, conn,
                       performed_queries_total, failed_queries_total);

  thd->myParam = &myParams;

  /* run pstress in with dynamic generator or infile */
  if (options->at(Option::PQUERY)->getBool() == false) {
    static bool success = false;

    /* load metadata */
    if (!lock_metadata.test_and_set()) {
      success = thd->load_metadata();
      metadata_loaded = true;
    }

    /* wait untill metadata is finished */
    while (!metadata_loaded) {
      std::chrono::seconds dura(3);
      std::this_thread::sleep_for(dura);
      thread_log << "waiting for metadata load to finish" << std::endl;
    }

    if (!success)
      thread_log << " initial setup failed, check logs for details "
                 << std::endl;
    else {
      if (!thd->run_some_query()) {
        std::ostringstream errmsg;
        errmsg << "Thread with id " << thd->thread_id
               << " failed, check logs  in " << myParams.logdir << "/*sql";
        std::cerr << errmsg.str() << std::endl;
        exit(EXIT_FAILURE);
        if (general_log.is_open()) {
          general_log << errmsg.str() << std::endl;
        }
      }
    }

  } else {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dis(0, querylist->size() - 1);
    int max_con_failures = 250;
    for (unsigned long i = 0; i < myParams.queries_per_thread; i++) {
      unsigned long query_number;
      // selecting query #, depends on random or sequential execution
      if (options->at(Option::NO_SHUFFLE)->getBool()) {
        query_number = i;
      } else {
        query_number = dis(gen);
      }
      // perform the query and getting the result
      execute_sql((*querylist)[query_number].c_str(), thd);

      if (thd->max_con_fail_count >= max_con_failures) {
        std::ostringstream errmsg;
        errmsg << "* Last " << thd->max_con_fail_count
               << " consecutive queries all failed. Likely crash/assert, user "
                  "privileges drop, or similar. Ending run.";
        std::cerr << errmsg.str() << std::endl;
        if (thread_log.is_open()) {
          thread_log << errmsg.str() << std::endl;
        }
        break;
      }
    }
  }
  /* connection can be changed if we thd->tryreconnect is called */
  conn = thd->conn;
  delete thd;
if (!log_all_queries) {
    // Retrieve recent queries from execute_sql
    std::deque<std::string> logDeque = get_recent_queries();
    // Trim to N queries if necessary
    if (options->at(Option::LOG_N_QUERIES) && 
      options->at(Option::LOG_N_QUERIES)->getInt() > 0) {
      size_t max_queries = options->at(Option::LOG_N_QUERIES)->getInt();
      while (logDeque.size() > max_queries) {
        logDeque.pop_front();
      }
    }
    // Write logs to file
    std::ofstream log_file_write(full_log_filename, std::ios::out | std::ios::trunc);
    if (!log_file_write.is_open()) {
        general_log << "Unable to open log file: " << full_log_filename << std::endl;
        return;
    }
    for (const auto &log : logDeque) {
        log_file_write << log << std::endl;
    }
    log_file_write.close();
  }

  if (thread_log.is_open())
    thread_log.close();

  if (client_log.is_open())
    client_log.close();

  mysql_close(conn);
  mysql_thread_end();
}

bool Thd1::tryreconnet() {
  MYSQL *conn;
  auto myParams = *this->myParam;
  conn = mysql_init(NULL);
  if (mysql_real_connect(conn, myParams.address.c_str(),
                         myParams.username.c_str(), myParams.password.c_str(),
                         myParams.database.c_str(), myParams.port,
                         myParams.socket.c_str(), 0) == NULL) {
    thread_log << "Error Failed to reconnect " << mysql_errno(conn);
    mysql_close(conn);

    return false;
  }
  MYSQL *old_conn = this->conn;
  mysql_close(old_conn);
  this->conn = conn;
  return true;
}

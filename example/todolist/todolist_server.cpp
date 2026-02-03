/*
 * Copyright 2025 NAVER Corp.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "httplib.h"
#include "pinpoint/tracer.h"
#include "http_trace_context.h"
#include <mysqlx/xdevapi.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <sstream>
#include <memory>
#include <string>
#include <vector>

using json = nlohmann::json;
using namespace pinpoint;
using namespace mysqlx;

// Thread local storage for span
thread_local pinpoint::SpanPtr current_span;

// Helper functions for thread local span management
void set_span_context(pinpoint::SpanPtr span) { current_span = span; }
pinpoint::SpanPtr get_span_context() { return current_span; }

// MySQL connection configuration
struct MySQLConfig {
    std::string host = "localhost";
    int port = 33060;  // X Protocol default port
    std::string user = "root";
    std::string password = "rootpassword";
    std::string database = "todolist";
    
    std::string GetConnectionString() const {
        std::stringstream ss;
        ss << "mysqlx://" << user << ":" << password 
           << "@" << host << ":" << port << "/" << database;
        return ss.str();
    }
};

// Global MySQL configuration and Pinpoint agent
MySQLConfig g_mysql_config;
AgentPtr g_agent;

// RAII MySQL session wrapper
class MySQLSession {
public:
    MySQLSession() {
        try {
            session_ = std::make_unique<Session>(g_mysql_config.GetConnectionString());
            
            // Set UTF-8
            session_->sql("SET NAMES utf8mb4").execute();
        } catch (const Error& err) {
            throw std::runtime_error(std::string("Failed to create MySQL session: ") + err.what());
        }
    }
    
    ~MySQLSession() = default;
    
    Session* get() { return session_.get(); }
    
    // Disable copy
    MySQLSession(const MySQLSession&) = delete;
    MySQLSession& operator=(const MySQLSession&) = delete;
    
private:
    std::unique_ptr<Session> session_;
};

// Execute query and return result
SqlResult execute_query(Session* session, const std::string& query, SpanPtr span = nullptr) {
    SpanEventPtr event;
    if (span) {
        event = span->NewSpanEvent("MySQL Query", SERVICE_TYPE_MYSQL_QUERY);
        event->SetSqlQuery(query, "");
        event->SetDestination("todolist");
        
        std::stringstream endpoint;
        endpoint << g_mysql_config.host << ":" << g_mysql_config.port;
        event->SetEndPoint(endpoint.str());
    }
    
    try {
        SqlResult result = session->sql(query).execute();
        
        if (event) {
            span->EndSpanEvent();
        }
        
        return result;
    } catch (const Error& e) {
        if (event) {
            event->SetError("MySQL Error", e.what());
            span->EndSpanEvent();
        }
        throw std::runtime_error(std::string("Query failed: ") + e.what());
    }
}

// Execute update/insert/delete and return affected rows
int execute_update(Session* session, const std::string& query, SpanPtr span = nullptr) {
    SpanEventPtr event;
    if (span) {
        event = span->NewSpanEvent("MySQL Query", SERVICE_TYPE_MYSQL_QUERY);
        event->SetSqlQuery(query, "");
        event->SetDestination("todolist");
        
        std::stringstream endpoint;
        endpoint << g_mysql_config.host << ":" << g_mysql_config.port;
        event->SetEndPoint(endpoint.str());
    }
    
    try {
        SqlResult result = session->sql(query).execute();
        int affected = result.getAffectedItemsCount();
        
        if (event) {
            span->EndSpanEvent();
        }
        
        return affected;
    } catch (const Error& e) {
        if (event) {
            event->SetError("MySQL Error", e.what());
            span->EndSpanEvent();
        }
        throw std::runtime_error(std::string("Query failed: ") + e.what());
    }
}

// HTTP tracing helper functions
pinpoint::SpanPtr trace_request(const httplib::Request& req) {
    auto agent = g_agent;
    if (!agent) {
        return nullptr;
    }

    HttpHeaderReader http_reader(req.headers);
    auto span = agent->NewSpan("C++ REST Server", req.path, req.method, http_reader);

    auto end_point = req.get_header_value("Host");
    if (end_point.empty()) {
        end_point = req.local_addr + ":" + std::to_string(req.local_port);
    }

    helper::TraceHttpServerRequest(span, req.remote_addr, end_point, http_reader);
    return span;
}

void trace_response(const httplib::Request& req, httplib::Response& res, pinpoint::SpanPtr span) {
    if (!span) {
        return;
    }

    HttpHeaderReader http_reader(res.headers);
    helper::TraceHttpServerResponse(span, req.matched_route.empty() ? req.path : req.matched_route, 
                                    req.method, res.status, http_reader);
    span->EndSpan();
}

httplib::Server::Handler wrap_handler(httplib::Server::Handler handler) {
    return [handler](const httplib::Request& req, httplib::Response& res) {
        auto span = trace_request(req);
        set_span_context(span);  // Store span in thread local storage
        
        handler(req, res);

        trace_response(req, res, span);        
        set_span_context(nullptr);  // Clear span from thread local storage
    };
}

// User API handlers
void handle_create_user(const httplib::Request& req, httplib::Response& res) {
    try {
        auto span = get_span_context();  // Get span from thread local storage
        
        // Parse JSON body
        json body = json::parse(req.body);
        std::string username = body["username"];
        std::string email = body["email"];
        std::string full_name = body.value("full_name", "");
        
        // Insert user
        MySQLSession session;
        std::stringstream query;
        query << "INSERT INTO users (username, email, full_name) VALUES ('"
              << username << "', '" << email << "', '" << full_name << "')";
        
        execute_update(session.get(), query.str(), span);
        
        // Get last insert ID
        SqlResult id_result = session.get()->sql("SELECT LAST_INSERT_ID()").execute();
        int user_id = 0;
        if (auto row = id_result.fetchOne()) {
            user_id = row[0].get<int>();
        }
        
        // Response
        json response = {
            {"status", "success"},
            {"user_id", user_id},
            {"username", username},
            {"email", email}
        };
        
        res.set_content(response.dump(), "application/json");
        res.status = 201;
        
    } catch (const std::exception& e) {
        json error = {{"status", "error"}, {"message", e.what()}};
        res.set_content(error.dump(), "application/json");
        res.status = 400;
    }
}

void handle_get_user(const httplib::Request& req, httplib::Response& res) {
    try {
        auto span = get_span_context();  // Get span from thread local storage
        int user_id = std::stoi(req.matches[1]);
        
        MySQLSession session;
        std::stringstream query;
        query << "SELECT id, username, email, full_name, created_at, updated_at "
              << "FROM users WHERE id = " << user_id;
        
        auto result = execute_query(session.get(), query.str(), span);
        
        if (auto row = result.fetchOne()) {
            json response = {
                {"id", row[0].get<int>()},
                {"username", row[1].get<std::string>()},
                {"email", row[2].get<std::string>()},
                {"full_name", row[3].isNull() ? "" : row[3].get<std::string>()},
                {"created_at", row[4].get<std::string>()},
                {"updated_at", row[5].get<std::string>()}
            };
            res.set_content(response.dump(), "application/json");
            res.status = 200;
        } else {
            json error = {{"status", "error"}, {"message", "User not found"}};
            res.set_content(error.dump(), "application/json");
            res.status = 404;
        }
        
    } catch (const std::exception& e) {
        json error = {{"status", "error"}, {"message", e.what()}};
        res.set_content(error.dump(), "application/json");
        res.status = 400;
    }
}

void handle_update_user(const httplib::Request& req, httplib::Response& res) {
    try {
        auto span = get_span_context();  // Get span from thread local storage
        int user_id = std::stoi(req.matches[1]);
        json body = json::parse(req.body);
        
        MySQLSession session;
        std::stringstream query;
        query << "UPDATE users SET ";
        
        bool first = true;
        if (body.contains("username")) {
            query << "username = '" << body["username"].get<std::string>() << "'";
            first = false;
        }
        if (body.contains("email")) {
            if (!first) query << ", ";
            query << "email = '" << body["email"].get<std::string>() << "'";
            first = false;
        }
        if (body.contains("full_name")) {
            if (!first) query << ", ";
            query << "full_name = '" << body["full_name"].get<std::string>() << "'";
        }
        
        query << " WHERE id = " << user_id;
        
        execute_update(session.get(), query.str(), span);
        
        json response = {{"status", "success"}, {"message", "User updated"}};
        res.set_content(response.dump(), "application/json");
        res.status = 200;
        
    } catch (const std::exception& e) {
        json error = {{"status", "error"}, {"message", e.what()}};
        res.set_content(error.dump(), "application/json");
        res.status = 400;
    }
}

void handle_delete_user(const httplib::Request& req, httplib::Response& res) {
    try {
        auto span = get_span_context();  // Get span from thread local storage
        int user_id = std::stoi(req.matches[1]);
        
        MySQLSession session;
        std::stringstream query;
        query << "DELETE FROM users WHERE id = " << user_id;
        
        execute_update(session.get(), query.str(), span);
        
        json response = {{"status", "success"}, {"message", "User deleted"}};
        res.set_content(response.dump(), "application/json");
        res.status = 200;
        
    } catch (const std::exception& e) {
        json error = {{"status", "error"}, {"message", e.what()}};
        res.set_content(error.dump(), "application/json");
        res.status = 400;
    }
}

// Todo API handlers
void handle_create_todo(const httplib::Request& req, httplib::Response& res) {
    try {
        auto span = get_span_context();  // Get span from thread local storage
        
        json body = json::parse(req.body);
        int user_id = body["user_id"];
        std::string title = body["title"];
        std::string description = body.value("description", "");
        std::string status = body.value("status", "pending");
        std::string priority = body.value("priority", "medium");
        std::string due_date = body.value("due_date", "NULL");
        
        MySQLSession session;
        std::stringstream query;
        query << "INSERT INTO todos (user_id, title, description, status, priority, due_date) VALUES ("
              << user_id << ", '" << title << "', '" << description << "', '"
              << status << "', '" << priority << "', ";
        
        if (due_date == "NULL") {
            query << "NULL)";
        } else {
            query << "'" << due_date << "')";
        }
        
        execute_update(session.get(), query.str(), span);
        
        // Get last insert ID
        SqlResult id_result = session.get()->sql("SELECT LAST_INSERT_ID()").execute();
        int todo_id = 0;
        if (auto row = id_result.fetchOne()) {
            todo_id = row[0].get<int>();
        }
        
        json response = {
            {"status", "success"},
            {"todo_id", todo_id},
            {"title", title}
        };
        
        res.set_content(response.dump(), "application/json");
        res.status = 201;
        
    } catch (const std::exception& e) {
        json error = {{"status", "error"}, {"message", e.what()}};
        res.set_content(error.dump(), "application/json");
        res.status = 400;
    }
}

void handle_get_todos(const httplib::Request& req, httplib::Response& res) {
    try {
        auto span = get_span_context();  // Get span from thread local storage
        
        MySQLSession session;
        std::stringstream query;
        query << "SELECT id, user_id, title, description, status, priority, due_date, "
              << "created_at, updated_at FROM todos";
        
        // Filter by user_id if provided
        if (req.has_param("user_id")) {
            int user_id = std::stoi(req.get_param_value("user_id"));
            query << " WHERE user_id = " << user_id;
        }
        
        // Filter by status if provided
        if (req.has_param("status")) {
            std::string status = req.get_param_value("status");
            query << (req.has_param("user_id") ? " AND" : " WHERE");
            query << " status = '" << status << "'";
        }
        
        query << " ORDER BY created_at DESC";
        
        auto result = execute_query(session.get(), query.str(), span);
        
        json todos = json::array();
        auto rows = result.fetchAll();
        for (const auto& row : rows) {
            json todo = {
                {"id", row[0].get<int>()},
                {"user_id", row[1].get<int>()},
                {"title", row[2].get<std::string>()},
                {"description", row[3].isNull() ? "" : row[3].get<std::string>()},
                {"status", row[4].get<std::string>()},
                {"priority", row[5].get<std::string>()},
                {"due_date", row[6].isNull() ? "" : row[6].get<std::string>()},
                {"created_at", row[7].get<std::string>()},
                {"updated_at", row[8].get<std::string>()}
            };
            todos.push_back(todo);
        }
        
        json response = {
            {"status", "success"},
            {"count", todos.size()},
            {"todos", todos}
        };
        
        res.set_content(response.dump(), "application/json");
        res.status = 200;
        
    } catch (const std::exception& e) {
        json error = {{"status", "error"}, {"message", e.what()}};
        res.set_content(error.dump(), "application/json");
        res.status = 400;
    }
}

void handle_get_todo(const httplib::Request& req, httplib::Response& res) {
    try {
        auto span = get_span_context();  // Get span from thread local storage
        int todo_id = std::stoi(req.matches[1]);
        
        MySQLSession session;
        std::stringstream query;
        query << "SELECT id, user_id, title, description, status, priority, due_date, "
              << "created_at, updated_at FROM todos WHERE id = " << todo_id;
        
        auto result = execute_query(session.get(), query.str(), span);
        
        if (auto row = result.fetchOne()) {
            json response = {
                {"id", row[0].get<int>()},
                {"user_id", row[1].get<int>()},
                {"title", row[2].get<std::string>()},
                {"description", row[3].isNull() ? "" : row[3].get<std::string>()},
                {"status", row[4].get<std::string>()},
                {"priority", row[5].get<std::string>()},
                {"due_date", row[6].isNull() ? "" : row[6].get<std::string>()},
                {"created_at", row[7].get<std::string>()},
                {"updated_at", row[8].get<std::string>()}
            };
            res.set_content(response.dump(), "application/json");
            res.status = 200;
        } else {
            json error = {{"status", "error"}, {"message", "Todo not found"}};
            res.set_content(error.dump(), "application/json");
            res.status = 404;
        }
        
    } catch (const std::exception& e) {
        json error = {{"status", "error"}, {"message", e.what()}};
        res.set_content(error.dump(), "application/json");
        res.status = 400;
    }
}

void handle_update_todo(const httplib::Request& req, httplib::Response& res) {
    try {
        auto span = get_span_context();  // Get span from thread local storage
        int todo_id = std::stoi(req.matches[1]);
        json body = json::parse(req.body);
        
        MySQLSession session;
        std::stringstream query;
        query << "UPDATE todos SET ";
        
        std::vector<std::string> updates;
        if (body.contains("title")) {
            updates.push_back("title = '" + body["title"].get<std::string>() + "'");
        }
        if (body.contains("description")) {
            updates.push_back("description = '" + body["description"].get<std::string>() + "'");
        }
        if (body.contains("status")) {
            updates.push_back("status = '" + body["status"].get<std::string>() + "'");
        }
        if (body.contains("priority")) {
            updates.push_back("priority = '" + body["priority"].get<std::string>() + "'");
        }
        if (body.contains("due_date")) {
            updates.push_back("due_date = '" + body["due_date"].get<std::string>() + "'");
        }
        
        for (size_t i = 0; i < updates.size(); ++i) {
            if (i > 0) query << ", ";
            query << updates[i];
        }
        
        query << " WHERE id = " << todo_id;
        
        execute_update(session.get(), query.str(), span);
        
        json response = {{"status", "success"}, {"message", "Todo updated"}};
        res.set_content(response.dump(), "application/json");
        res.status = 200;
        
    } catch (const std::exception& e) {
        json error = {{"status", "error"}, {"message", e.what()}};
        res.set_content(error.dump(), "application/json");
        res.status = 400;
    }
}

void handle_delete_todo(const httplib::Request& req, httplib::Response& res) {
    try {
        auto span = get_span_context();  // Get span from thread local storage
        int todo_id = std::stoi(req.matches[1]);
        
        MySQLSession session;
        std::stringstream query;
        query << "DELETE FROM todos WHERE id = " << todo_id;
        
        execute_update(session.get(), query.str(), span);
        
        json response = {{"status", "success"}, {"message", "Todo deleted"}};
        res.set_content(response.dump(), "application/json");
        res.status = 200;
        
    } catch (const std::exception& e) {
        json error = {{"status", "error"}, {"message", e.what()}};
        res.set_content(error.dump(), "application/json");
        res.status = 400;
    }
}

void handle_reconfig_sampling(const httplib::Request& req, httplib::Response& res) {
    if (!g_agent) {
        json error = {{"status", "error"}, {"message", "Agent not initialized"}};
        res.set_content(error.dump(), "application/json");
        res.status = 500;
        return;
    }

    if (!req.has_param("sampling_rate")) {
        json error = {{"status", "error"}, {"message", "Missing sampling_rate parameter"}};
        res.set_content(error.dump(), "application/json");
        res.status = 400;
        return;
    }

    std::string sampling_rate = req.get_param_value("sampling_rate");
    setenv("PINPOINT_CPP_SAMPLING_TYPE", "PERCENT", 0);
    setenv("PINPOINT_CPP_SAMPLING_PERCENT_RATE", sampling_rate.c_str(), 0);

    g_agent = pinpoint::CreateAgent();
    
    json response = {
        {"status", "success"},
        {"sampling_type", "PERCENT"},
        {"sampling_rate", sampling_rate}
    };
    res.set_content(response.dump(), "application/json");
    res.status = 200;
}

int main(int argc, char* argv[]) {
    // Parse command line arguments
    if (argc > 1) {
        SetConfigFilePath(argv[1]);
    } else {
        SetConfigFilePath("pinpoint-config.yaml");
    }

    setenv("PINPOINT_CPP_APPLICATION_NAME", "cpp-todolist", 0);
    setenv("PINPOINT_CPP_SQL_ENABLE_SQL_STATS", "true", 0);
    setenv("PINPOINT_CPP_HTTP_COLLECT_URL_STAT", "true", 0);
    setenv("PINPOINT_CPP_LOG_LEVEL", "debug", 0);
    
    // Test MySQL connection
    try {
        MySQLSession test_session;
        std::cout << "MySQL X DevAPI connection successful" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "MySQL connection test failed: " << e.what() << std::endl;
        std::cerr << "Please ensure MySQL X Protocol is enabled and running on port " 
                  << g_mysql_config.port << std::endl;
        return 1;
    }
    
    // Initialize Pinpoint agent
    g_agent = pinpoint::CreateAgent();
    // Create HTTP server
    httplib::Server svr;
    
    // Set timeout
    svr.set_read_timeout(5, 0);
    svr.set_write_timeout(5, 0);
    
    // Register handlers with tracing wrapper
    // User APIs
    svr.Post("/api/users", wrap_handler(handle_create_user));
    svr.Get(R"(/api/users/(\d+))", wrap_handler(handle_get_user));
    svr.Put(R"(/api/users/(\d+))", wrap_handler(handle_update_user));
    svr.Delete(R"(/api/users/(\d+))", wrap_handler(handle_delete_user));
    
    // Todo APIs
    svr.Post("/api/todos", wrap_handler(handle_create_todo));
    svr.Get("/api/todos", wrap_handler(handle_get_todos));
    svr.Get(R"(/api/todos/(\d+))", wrap_handler(handle_get_todo));
    svr.Put(R"(/api/todos/(\d+))", wrap_handler(handle_update_todo));
    svr.Delete(R"(/api/todos/(\d+))", wrap_handler(handle_delete_todo));

    // Reconfigure sampling rate (example)
    svr.Post("/reconfig", wrap_handler(handle_reconfig_sampling));
    
    // Health check
    svr.Get("/health", wrap_handler([](const httplib::Request&, httplib::Response& res) {
        json response = {{"status", "healthy"}, {"service", "todolist"}};
        res.set_content(response.dump(), "application/json");
    }));
    
    // Root endpoint
    svr.Get("/", wrap_handler([](const httplib::Request&, httplib::Response& res) {
        json response = {
            {"service", "TodoList API"},
            {"version", "1.0.0"},
            {"endpoints", {
                {"users", {
                    {"POST /api/users", "Create user"},
                    {"GET /api/users/:id", "Get user"},
                    {"PUT /api/users/:id", "Update user"},
                    {"DELETE /api/users/:id", "Delete user"}
                }},
                {"todos", {
                    {"POST /api/todos", "Create todo"},
                    {"GET /api/todos", "List todos (query: user_id, status)"},
                    {"GET /api/todos/:id", "Get todo"},
                    {"PUT /api/todos/:id", "Update todo"},
                    {"DELETE /api/todos/:id", "Delete todo"}
                }},
                {"config", {
                    {"POST /reconfig?sampling_rate=10.0", "Update sampling rate (percent)"}
                }}
            }}
        };
        res.set_content(response.dump(2), "application/json");
    }));
    
    std::cout << "\nTodoList Server Starting..." << std::endl;
    std::cout << "=============================" << std::endl;
    std::cout << "Server: http://localhost:8080" << std::endl;
    std::cout << "MySQL X Protocol: " << g_mysql_config.host << ":" << g_mysql_config.port << std::endl;
    std::cout << "Press Ctrl+C to stop" << std::endl;
    std::cout << "=============================" << std::endl << std::endl;
    
    // Start server
    if (!svr.listen("0.0.0.0", 8080)) {
        std::cerr << "Failed to start server" << std::endl;
        return 1;
    }
    
    // Cleanup
    if (g_agent) {
        g_agent->Shutdown();
    }
    
    return 0;
}


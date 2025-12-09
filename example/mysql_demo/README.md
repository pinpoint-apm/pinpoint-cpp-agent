# MySQL Demo

This example demonstrates the Pinpoint C++ Agent with MySQL database instrumentation using MySQL Connector/C++ X DevAPI.

## Features

- HTTP server with database query endpoints
- MySQL database integration with X DevAPI
- SQL query tracing and statistics
- Transaction management
- Error handling and reporting

## Prerequisites

- MySQL Connector/C++ 8.0 or later
- MySQL Server 8.0 or later
- C++17 compatible compiler
- CMake 3.14+
- Docker (for containerized deployment)

## Building Locally

### Install MySQL Connector/C++

**macOS (with Homebrew):**
```bash
brew install mysql-connector-c++
```

**Ubuntu/Debian:**
```bash
# Download from MySQL official site
wget https://dev.mysql.com/get/Downloads/Connector-C++/mysql-connector-c++-8.0.33-linux-glibc2.28-x86-64bit.tar.gz
tar -xzf mysql-connector-c++-8.0.33-linux-glibc2.28-x86-64bit.tar.gz
cd mysql-connector-c++-8.0.33-linux-glibc2.28-x86-64bit
sudo cp -r include/* /usr/local/include/
sudo cp -r lib64/* /usr/local/lib/
sudo ldconfig
```

### Build the Demo

```bash
cd ../../..
mkdir -p build && cd build
cmake -DBUILD_EXAMPLES=ON -DBUILD_MYSQL_EXAMPLE=ON ..
make -j$(nproc)
```

The executable will be at `build/example/mysql_demo/db_demo`

## Running with Docker

The easiest way to run this demo is with Docker Compose:

```bash
cd example/mysql_demo
docker-compose up --build
```

This will:
- Start a MySQL 8.0 server with test database
- Initialize the database with sample schema (users table)
- Build and run the db_demo server on port 8089

## Usage

### Start the Server

**Local:**
```bash
# Start MySQL first
docker-compose up -d mysql

# Set environment variables
export MYSQL_HOST=localhost
export MYSQL_PORT=33060
export MYSQL_DATABASE=test
export MYSQL_USER=root
export MYSQL_PASSWORD=pinpoint123

# Run the demo
./build/example/mysql_demo/db_demo
```

**Docker:**
```bash
docker-compose up
```

### Test Endpoints

**1. Status endpoint:**
```bash
curl http://localhost:8089/status
```

Response:
```json
{
  "service": "MySQL Database Demo",
  "status": "running",
  "mysql_connected": true,
  "endpoints": [
    "GET /status - Service status",
    "GET /db-demo - Run database demo operations"
  ]
}
```

**2. Database demo endpoint:**
```bash
curl http://localhost:8089/db-demo
```

This endpoint performs various database operations:
- Create users table (if not exists)
- Insert sample users
- Query users
- Update user data
- Delete users
- Transaction management

Response:
```json
{
  "success": true,
  "operations": [
    {
      "operation": "create_table",
      "status": "success",
      "duration_ms": 45
    },
    {
      "operation": "insert_users",
      "status": "success",
      "rows_affected": 3,
      "duration_ms": 23
    },
    {
      "operation": "select_users",
      "status": "success",
      "rows_count": 3,
      "duration_ms": 12
    }
  ]
}
```

## Configuration

### Environment Variables

Required environment variables for database connection:
- `MYSQL_HOST`: MySQL server host (default: `localhost`)
- `MYSQL_PORT`: MySQL X Protocol port (default: `33060`)
- `MYSQL_DATABASE`: Database name (default: `test`)
- `MYSQL_USER`: Database user (default: `root`)
- `MYSQL_PASSWORD`: Database password

### Pinpoint Configuration

Edit `pinpoint-config.yaml` to configure:
- Application name and type
- Collector host and ports
- Sampling strategy
- SQL statistics
- Logging level

## Database Schema

The demo uses a simple `users` table:

```sql
CREATE TABLE IF NOT EXISTS users (
    id INT AUTO_INCREMENT PRIMARY KEY,
    name VARCHAR(100) NOT NULL,
    email VARCHAR(100) NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
```

## How It Works

### SQL Query Tracing

Each database operation is traced using SpanEvents:

```cpp
auto se = span->NewSpanEvent("select_users");
se->SetServiceType(pinpoint::SERVICE_TYPE_MYSQL_QUERY);
se->SetEndPoint("mysql:33060");
se->SetDestination("test");

// Record SQL query
std::string sql = "SELECT * FROM users";
se->SetSqlQuery(sql);

try {
    // Execute query
    mysqlx::SqlResult result = session.sql(sql).execute();
    // Process results...
} catch (const std::exception& e) {
    se->SetError(e.what());
    throw;
}

span->EndSpanEvent();
```

### Transaction Management

The demo shows how to trace database transactions:

```cpp
auto se = span->NewSpanEvent("transaction");
session.startTransaction();

try {
    // Insert operations
    session.sql("INSERT INTO users ...").execute();
    session.sql("UPDATE users ...").execute();
    
    session.commit();
    se->GetAnnotations()->AppendString("tx_status", "committed");
} catch (const std::exception& e) {
    session.rollback();
    se->SetError(e.what());
    se->GetAnnotations()->AppendString("tx_status", "rolled_back");
    throw;
}

span->EndSpanEvent();
```

### HTTP Server Integration

The demo uses cpp-httplib to provide HTTP endpoints:

```cpp
httplib::Server server;

server.Get("/db-demo", [](const httplib::Request& req, httplib::Response& res) {
    auto agent = pinpoint::GlobalAgent();
    auto span = agent->NewSpan("MySQL Demo", req.path);
    
    // Database operations...
    
    span->EndSpan();
});

server.listen("0.0.0.0", 8089);
```

## Architecture

```
┌─────────────────┐
│   HTTP Client   │
└────────┬────────┘
         │ GET /db-demo
         ▼
┌─────────────────┐
│   db_demo.cpp   │
│  (HTTP Server)  │
└────────┬────────┘
         │ SQL Queries
         ▼
┌─────────────────┐
│  MySQL Server   │
│   (Docker)      │
└─────────────────┘
         │
         ▼
┌─────────────────┐
│   Pinpoint      │
│   Collector     │
└─────────────────┘
```

## Troubleshooting

### MySQL Connection Failed

```
Error: Unable to connect to MySQL server
```

**Solution**:
- Check if MySQL is running: `docker-compose ps`
- Verify connection parameters (host, port, credentials)
- Check MySQL logs: `docker-compose logs mysql`
- Ensure X Protocol is enabled (port 33060)

### Table Already Exists

```
Error: Table 'users' already exists
```

**Solution**: This is normal - the demo uses `CREATE TABLE IF NOT EXISTS`

### Build Failed - MySQL Connector Not Found

```
CMake Error: Could not find MySQL Connector/C++
```

**Solution**: Install MySQL Connector/C++ (see prerequisites)

### Port Already in Use

```
bind: Address already in use
```

**Solution**: Change the port in `db_demo.cpp`:
```cpp
server.listen("0.0.0.0", 8090);  // Use different port
```

Or stop the conflicting service:
```bash
lsof -ti:8089 | xargs kill
```

## Performance Considerations

### Connection Pooling

For production use, consider implementing connection pooling:
- Reuse database sessions
- Limit maximum connections
- Set connection timeout

### Query Optimization

- Use prepared statements when possible
- Add appropriate indexes to tables
- Avoid N+1 query problems
- Use transactions for bulk operations

### Monitoring

Check Pinpoint Web UI for:
- SQL query performance
- Slow query detection
- Database connection statistics
- Transaction metrics

## Examples of Database Operations

### Insert Operation

```cpp
auto se = span->NewSpanEvent("insert_user");
se->SetServiceType(pinpoint::SERVICE_TYPE_MYSQL_QUERY);

std::string sql = "INSERT INTO users (name, email) VALUES (?, ?)";
se->SetSqlQuery(sql);

session.sql(sql)
    .bind("John Doe")
    .bind("john@example.com")
    .execute();

span->EndSpanEvent();
```

### Select Operation

```cpp
auto se = span->NewSpanEvent("select_users");
se->SetServiceType(pinpoint::SERVICE_TYPE_MYSQL_QUERY);

std::string sql = "SELECT * FROM users WHERE id > ?";
se->SetSqlQuery(sql);

auto result = session.sql(sql).bind(100).execute();

// Process rows
auto rows = result.fetchAll();
for (const auto& row : rows) {
    std::cout << "User: " << row[1].get<std::string>() << std::endl;
}

span->EndSpanEvent();
```

### Update Operation

```cpp
auto se = span->NewSpanEvent("update_user");
se->SetServiceType(pinpoint::SERVICE_TYPE_MYSQL_QUERY);

std::string sql = "UPDATE users SET email = ? WHERE id = ?";
se->SetSqlQuery(sql);

auto result = session.sql(sql)
    .bind("newemail@example.com")
    .bind(1)
    .execute();

auto affected = result.getAffectedItemsCount();
se->GetAnnotations()->AppendInt("rows_affected", affected);

span->EndSpanEvent();
```

## See Also

- [Pinpoint C++ Agent Documentation](../../doc/)
- [MySQL Connector/C++ X DevAPI Documentation](https://dev.mysql.com/doc/x-devapi-userguide/en/)
- [cpp-httplib](https://github.com/yhirose/cpp-httplib)


#!/bin/bash

# MySQL Database Demo with Docker
# This script starts MySQL in Docker and runs the db_demo

set -e  # Exit on any error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Configuration
MYSQL_CONTAINER="pinpoint-mysql"
DB_DEMO_PORT="8089"

echo -e "${BLUE}===============================================${NC}"
echo -e "${BLUE} MySQL Database Demo with Docker Setup${NC}"
echo -e "${BLUE}===============================================${NC}"
echo

# Function to check if MySQL is ready
wait_for_mysql() {
    echo -e "${YELLOW}Waiting for MySQL to be ready...${NC}"
    
    for i in {1..30}; do
        if docker exec $MYSQL_CONTAINER mysqladmin ping -h localhost -uroot -ppinpoint123 --silent > /dev/null 2>&1; then
            echo -e "${GREEN}✓ MySQL is ready!${NC}"
            return 0
        fi
        echo -n "."
        sleep 2
    done
    
    echo -e "${RED}✗ MySQL failed to start within 60 seconds${NC}"
    return 1
}

# Function to cleanup
cleanup() {
    echo -e "\n${YELLOW}Cleaning up...${NC}"
    docker-compose -f $(dirname $0)/docker-compose.yml down
    echo -e "${GREEN}Cleanup completed${NC}"
}

# Function to show MySQL connection info
show_connection_info() {
    echo -e "${BLUE}MySQL Connection Information:${NC}"
    echo "  Host: localhost"
    echo "  Port: 3306 (MySQL) / 33060 (X Protocol)"
    echo "  Database: test" 
    echo "  Username: root / pinpoint"
    echo "  Password: pinpoint123"
    echo
}

# Function to test MySQL connection
test_mysql_connection() {
    echo -e "${YELLOW}Testing MySQL connection...${NC}"
    
    if docker exec $MYSQL_CONTAINER mysql -h localhost -uroot -ppinpoint123 -e "SELECT 'MySQL connection successful' as status, VERSION() as version;" test; then
        echo -e "${GREEN}✓ MySQL connection test passed${NC}"
        return 0
    else
        echo -e "${RED}✗ MySQL connection test failed${NC}"
        return 1
    fi
}

# Trap to cleanup on exit
trap cleanup EXIT INT TERM

# Check if Docker is running
if ! docker info > /dev/null 2>&1; then
    echo -e "${RED}✗ Docker is not running. Please start Docker first.${NC}"
    exit 1
fi

# Change to script directory
cd "$(dirname "$0")"

# Start MySQL with Docker Compose
echo -e "${YELLOW}Starting MySQL container...${NC}"
docker-compose up -d

# Wait for MySQL to be ready
if ! wait_for_mysql; then
    echo -e "${RED}Failed to start MySQL. Check Docker logs:${NC}"
    docker-compose logs mysql
    exit 1
fi

# Show connection info
show_connection_info

# Test MySQL connection
if ! test_mysql_connection; then
    echo -e "${RED}MySQL connection test failed${NC}"
    exit 1
fi

# Check if db_demo exists and is executable
DB_DEMO_PATH="../../build/example/mysql_demo/db_demo"
if [ ! -f "$DB_DEMO_PATH" ]; then
    echo -e "${RED}✗ db_demo not found at $DB_DEMO_PATH${NC}"
    echo -e "${YELLOW}Please build the project first:${NC}"
    echo "  cd ../.. && cmake --build build --target db_demo"
    exit 1
fi

if [ ! -x "$DB_DEMO_PATH" ]; then
    echo -e "${RED}✗ db_demo is not executable${NC}"
    chmod +x "$DB_DEMO_PATH"
fi

# Start db_demo
echo -e "${GREEN}✓ Starting db_demo HTTP server...${NC}"
echo -e "${BLUE}Database Demo will be available at:${NC}"
echo "  Status: http://localhost:$DB_DEMO_PORT/status"
echo "  Demo:   http://localhost:$DB_DEMO_PORT/db-demo"
echo
echo -e "${YELLOW}Press Ctrl+C to stop both db_demo and MySQL${NC}"
echo

# Export environment variables for db_demo to connect to Docker MySQL
# Note: MySQL X Protocol uses port 33060, not 3306
export MYSQL_HOST="localhost"
export MYSQL_PORT="33060"
export MYSQL_DATABASE="test" 
export MYSQL_USER="root"
export MYSQL_PASSWORD="pinpoint123"

# Run db_demo (this will block until interrupted)
$DB_DEMO_PATH

echo -e "${GREEN}db_demo stopped${NC}"

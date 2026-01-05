#!/bin/bash

# TodoList Service Startup Script
# ================================

set -e

BOLD='\033[1m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

echo -e "${BOLD}TodoList Service Startup${NC}"
echo "=========================="
echo ""

# Check if Docker is running
if ! docker info > /dev/null 2>&1; then
    echo -e "${RED}Error: Docker is not running${NC}"
    echo "Please start Docker and try again"
    exit 1
fi

# Start MySQL
echo -e "${BLUE}[1/4]${NC} Starting MySQL database..."
docker-compose up -d mysql

# Wait for MySQL to be ready
echo -e "${BLUE}[2/4]${NC} Waiting for MySQL to be ready..."
for i in {1..30}; do
    if docker-compose exec -T mysql mysqladmin ping -h localhost -u root -prootpassword > /dev/null 2>&1; then
        echo -e "${GREEN}✓ MySQL is ready${NC}"
        break
    fi
    if [ $i -eq 30 ]; then
        echo -e "${RED}✗ MySQL failed to start${NC}"
        docker-compose logs mysql
        exit 1
    fi
    echo -n "."
    sleep 1
done
echo ""

# Build the server if not already built
if [ ! -f "../../build/example/todolist/todolist_server" ]; then
    echo -e "${BLUE}[3/4]${NC} Building TodoList server..."
    cd ../../
    mkdir -p build
    cd build
    cmake -DBUILD_TODOLIST_EXAMPLE=ON ..
    make todolist_server
    cd ../example/todolist
    echo -e "${GREEN}✓ Server built successfully${NC}"
else
    echo -e "${BLUE}[3/4]${NC} Using existing build"
fi

# Start the server
echo -e "${BLUE}[4/4]${NC} Starting TodoList server..."
echo ""
echo -e "${GREEN}Services started!${NC}"
echo ""
echo -e "${BOLD}Service Information:${NC}"
echo "  TodoList API: http://localhost:8080"
echo "  MySQL: localhost:3306"
echo "    Database: todolist"
echo "    User: todouser"
echo "    Password: todopass"
echo ""
echo -e "${BOLD}Quick Commands:${NC}"
echo "  Health Check: curl http://localhost:8080/health"
echo "  API Info: curl http://localhost:8080/"
echo "  Run Tests: ./test.sh"
echo ""
echo -e "${YELLOW}Starting server...${NC}"
echo "Press Ctrl+C to stop"
echo ""

# Run the server
../../build/example/todolist/todolist_server pinpoint-config.yaml


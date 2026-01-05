#!/bin/bash

# TodoList API Test Script
# =========================

set -e

BASE_URL="http://localhost:8080"
BOLD='\033[1m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
RED='\033[0;31m'
NC='\033[0m' # No Color

echo -e "${BOLD}TodoList API Test Script${NC}"
echo "=========================="
echo ""

# Helper function to make requests
function api_test() {
    local method=$1
    local endpoint=$2
    local data=$3
    local desc=$4
    
    echo -e "${BLUE}[TEST]${NC} $desc"
    echo -e "${BOLD}$method $endpoint${NC}"
    
    if [ -n "$data" ]; then
        echo "Data: $data"
        response=$(curl -s -X $method "$BASE_URL$endpoint" \
            -H "Content-Type: application/json" \
            -d "$data" \
            -w "\nHTTP_CODE:%{http_code}")
    else
        response=$(curl -s -X $method "$BASE_URL$endpoint" \
            -w "\nHTTP_CODE:%{http_code}")
    fi
    
    # Extract HTTP code
    http_code=$(echo "$response" | grep "HTTP_CODE:" | cut -d':' -f2)
    body=$(echo "$response" | sed '/HTTP_CODE:/d')
    
    echo -e "${GREEN}Response ($http_code):${NC}"
    echo "$body" | python3 -m json.tool 2>/dev/null || echo "$body"
    echo ""
    
    # Check if successful
    if [[ $http_code -ge 200 && $http_code -lt 300 ]]; then
        echo -e "${GREEN}✓ Success${NC}"
    else
        echo -e "${RED}✗ Failed${NC}"
    fi
    echo ""
    echo "---"
    echo ""
}

# Wait for server to be ready
echo "Waiting for server to be ready..."
for i in {1..30}; do
    if curl -s "$BASE_URL/health" > /dev/null 2>&1; then
        echo -e "${GREEN}Server is ready!${NC}"
        echo ""
        break
    fi
    if [ $i -eq 30 ]; then
        echo -e "${RED}Server not responding. Please start the server first.${NC}"
        exit 1
    fi
    sleep 1
done

# Health Check
api_test "GET" "/health" "" "Health Check"

# Root endpoint
api_test "GET" "/" "" "Get API Information"

# User Tests
echo -e "${BOLD}=== User API Tests ===${NC}"
echo ""

# Create users
api_test "POST" "/api/users" \
    '{"username": "testuser1", "email": "test1@example.com", "full_name": "Test User 1"}' \
    "Create User 1"

api_test "POST" "/api/users" \
    '{"username": "testuser2", "email": "test2@example.com", "full_name": "Test User 2"}' \
    "Create User 2"

# Get user (assuming user_id 1 exists)
api_test "GET" "/api/users/1" "" "Get User 1"

# Update user
api_test "PUT" "/api/users/1" \
    '{"full_name": "Updated Test User"}' \
    "Update User 1"

# Get updated user
api_test "GET" "/api/users/1" "" "Get Updated User 1"

# Todo Tests
echo -e "${BOLD}=== Todo API Tests ===${NC}"
echo ""

# Create todos
api_test "POST" "/api/todos" \
    '{"user_id": 1, "title": "Test Todo 1", "description": "This is a test todo", "status": "pending", "priority": "high"}' \
    "Create Todo 1"

api_test "POST" "/api/todos" \
    '{"user_id": 1, "title": "Test Todo 2", "description": "Another test todo", "status": "in_progress", "priority": "medium", "due_date": "2025-12-31"}' \
    "Create Todo 2"

api_test "POST" "/api/todos" \
    '{"user_id": 2, "title": "User 2 Todo", "description": "Todo for user 2", "status": "pending", "priority": "low"}' \
    "Create Todo for User 2"

# Get all todos
api_test "GET" "/api/todos" "" "Get All Todos"

# Get todos for user 1
api_test "GET" "/api/todos?user_id=1" "" "Get Todos for User 1"

# Get todos by status
api_test "GET" "/api/todos?status=pending" "" "Get Pending Todos"

# Get todos with multiple filters
api_test "GET" "/api/todos?user_id=1&status=pending" "" "Get Pending Todos for User 1"

# Get specific todo (assuming todo_id 1 exists)
api_test "GET" "/api/todos/1" "" "Get Todo 1"

# Update todo
api_test "PUT" "/api/todos/1" \
    '{"status": "completed", "description": "Updated description"}' \
    "Update Todo 1"

# Get updated todo
api_test "GET" "/api/todos/1" "" "Get Updated Todo 1"

# Delete todo
api_test "DELETE" "/api/todos/1" "" "Delete Todo 1"

# Verify deletion
api_test "GET" "/api/todos/1" "" "Verify Todo 1 Deleted (should be 404)"

# Cleanup - Delete test user
api_test "DELETE" "/api/users/4" "" "Delete Test User (if exists)"
api_test "DELETE" "/api/users/5" "" "Delete Test User (if exists)"

echo ""
echo -e "${BOLD}${GREEN}All tests completed!${NC}"
echo ""
echo "Note: Some tests may fail if data doesn't exist. This is expected."


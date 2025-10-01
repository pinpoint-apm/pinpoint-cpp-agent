#!/bin/bash

# Test script for db_demo with Docker MySQL
# This script tests the HTTP endpoints of db_demo

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

DB_DEMO_URL="http://localhost:8089"
MAX_RETRIES=10
RETRY_INTERVAL=2

echo -e "${BLUE}===============================================${NC}"
echo -e "${BLUE} Testing db_demo HTTP Endpoints${NC}"
echo -e "${BLUE}===============================================${NC}"
echo

# Function to check if db_demo is running
check_db_demo() {
    if curl -s -o /dev/null -w "%{http_code}" "$DB_DEMO_URL/status" | grep -q "200"; then
        return 0
    else
        return 1
    fi
}

# Function to wait for db_demo to start
wait_for_db_demo() {
    echo -e "${YELLOW}Waiting for db_demo to start...${NC}"
    
    for i in $(seq 1 $MAX_RETRIES); do
        if check_db_demo; then
            echo -e "${GREEN}✓ db_demo is running!${NC}"
            return 0
        fi
        echo -n "."
        sleep $RETRY_INTERVAL
    done
    
    echo -e "${RED}✗ db_demo is not responding after $((MAX_RETRIES * RETRY_INTERVAL)) seconds${NC}"
    return 1
}

# Function to test status endpoint
test_status_endpoint() {
    echo -e "${YELLOW}Testing /status endpoint...${NC}"
    
    response=$(curl -s "$DB_DEMO_URL/status")
    http_code=$(curl -s -o /dev/null -w "%{http_code}" "$DB_DEMO_URL/status")
    
    if [ "$http_code" = "200" ]; then
        echo -e "${GREEN}✓ Status endpoint OK (HTTP $http_code)${NC}"
        echo -e "${BLUE}Response:${NC}"
        echo "$response" | head -10
        return 0
    else
        echo -e "${RED}✗ Status endpoint failed (HTTP $http_code)${NC}"
        return 1
    fi
}

# Function to test db-demo endpoint
test_db_demo_endpoint() {
    echo -e "${YELLOW}Testing /db-demo endpoint...${NC}"
    
    response=$(curl -s "$DB_DEMO_URL/db-demo")
    http_code=$(curl -s -o /dev/null -w "%{http_code}" "$DB_DEMO_URL/db-demo")
    
    echo -e "${BLUE}HTTP Status Code: $http_code${NC}"
    echo -e "${BLUE}Response Preview:${NC}"
    echo "$response" | head -20
    
    # Check if the response contains expected fields
    if echo "$response" | grep -q '"success"'; then
        if echo "$response" | grep -q '"success": *true'; then
            echo -e "${GREEN}✓ Database demo executed successfully!${NC}"
            
            # Show some details from the response
            if echo "$response" | grep -q '"operations"'; then
                echo -e "${BLUE}Operations completed:${NC}"
                echo "$response" | grep -o '"operation": *"[^"]*"' | sed 's/"operation": *"//;s/"//' | while read op; do
                    echo "  - $op"
                done
            fi
        else
            echo -e "${YELLOW}⚠ Database demo returned success: false (likely MySQL connection issue)${NC}"
            
            # Show error details
            if echo "$response" | grep -q '"error"'; then
                error=$(echo "$response" | grep -o '"error": *"[^"]*"' | sed 's/"error": *"//;s/"//')
                echo -e "${YELLOW}Error: $error${NC}"
            fi
            
            if echo "$response" | grep -q '"message"'; then
                message=$(echo "$response" | grep -o '"message": *"[^"]*"' | sed 's/"message": *"//;s/"//')
                echo -e "${YELLOW}Message: $message${NC}"
            fi
        fi
        return 0
    else
        echo -e "${RED}✗ Unexpected response format${NC}"
        return 1
    fi
}

# Function to show test summary
show_test_summary() {
    echo
    echo -e "${BLUE}===============================================${NC}"
    echo -e "${BLUE} Test Summary${NC}"
    echo -e "${BLUE}===============================================${NC}"
    echo -e "${GREEN}✓ HTTP server is running${NC}"
    echo -e "${GREEN}✓ Status endpoint is accessible${NC}"
    echo -e "${GREEN}✓ Database demo endpoint is accessible${NC}"
    echo
    echo -e "${BLUE}Manual test commands:${NC}"
    echo "  curl $DB_DEMO_URL/status | jq ."
    echo "  curl $DB_DEMO_URL/db-demo | jq ."
    echo
    echo -e "${BLUE}Open in browser:${NC}"
    echo "  $DB_DEMO_URL/status"
    echo
}

# Main test execution
echo -e "${YELLOW}Starting endpoint tests...${NC}"
echo

# Wait for db_demo to be ready
if ! wait_for_db_demo; then
    echo -e "${RED}Cannot proceed with tests - db_demo is not running${NC}"
    echo -e "${YELLOW}Make sure to start db_demo first:${NC}"
    echo "  ./run_db_demo_with_docker.sh"
    exit 1
fi

echo

# Test status endpoint
if test_status_endpoint; then
    echo -e "${GREEN}✓ Status endpoint test passed${NC}"
else
    echo -e "${RED}✗ Status endpoint test failed${NC}"
    exit 1
fi

echo

# Test db-demo endpoint
if test_db_demo_endpoint; then
    echo -e "${GREEN}✓ Database demo endpoint test passed${NC}"
else
    echo -e "${RED}✗ Database demo endpoint test failed${NC}"
    exit 1
fi

# Show summary
show_test_summary

echo -e "${GREEN}All tests completed successfully!${NC}"

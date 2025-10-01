-- Initialize MySQL database for Pinpoint C++ Demo
-- This script will be executed when MySQL container starts

-- Create test database (already created by MYSQL_DATABASE env var)
USE test;

-- Grant all privileges to pinpoint user
GRANT ALL PRIVILEGES ON *.* TO 'pinpoint'@'%';
GRANT ALL PRIVILEGES ON *.* TO 'root'@'%';
FLUSH PRIVILEGES;

-- Create demo table (will be recreated by demo, but useful for manual testing)
CREATE TABLE IF NOT EXISTS demo_users (
    id INT AUTO_INCREMENT PRIMARY KEY,
    name VARCHAR(100) NOT NULL,
    email VARCHAR(100) UNIQUE,
    age INT,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Insert some initial test data
INSERT IGNORE INTO demo_users (name, email, age) VALUES 
    ('Test User 1', 'test1@example.com', 25),
    ('Test User 2', 'test2@example.com', 30),
    ('Test User 3', 'test3@example.com', 35);

-- Verify the setup
SELECT 'Database initialization completed successfully' as message;
SELECT COUNT(*) as initial_user_count FROM demo_users;

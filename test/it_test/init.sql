-- Integration test database initialization
USE test;

-- Grant privileges
GRANT ALL PRIVILEGES ON *.* TO 'root'@'%';
FLUSH PRIVILEGES;

-- Pre-create tables used by the integration test server
CREATE TABLE IF NOT EXISTS it_test_users (
    id INT AUTO_INCREMENT PRIMARY KEY,
    name VARCHAR(100) NOT NULL,
    email VARCHAR(100),
    age INT,
    ts TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS it_test_batch (
    id INT AUTO_INCREMENT PRIMARY KEY,
    val VARCHAR(100),
    num INT
);

CREATE TABLE IF NOT EXISTS it_test_orders (
    id INT AUTO_INCREMENT PRIMARY KEY,
    user_id INT,
    amount DECIMAL(10,2),
    status VARCHAR(20),
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Seed some initial data
INSERT IGNORE INTO it_test_users (name, email, age) VALUES
    ('Seed User 1', 'seed1@test.com', 25),
    ('Seed User 2', 'seed2@test.com', 30),
    ('Seed User 3', 'seed3@test.com', 40);

SELECT 'Integration test DB initialized' AS message;

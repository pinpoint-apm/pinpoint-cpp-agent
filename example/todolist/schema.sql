-- TodoList Database Schema
-- ========================

-- Create database if not exists
CREATE DATABASE IF NOT EXISTS todolist DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

USE todolist;

-- Users table
CREATE TABLE IF NOT EXISTS users (
    id INT AUTO_INCREMENT PRIMARY KEY,
    username VARCHAR(50) NOT NULL UNIQUE,
    email VARCHAR(100) NOT NULL UNIQUE,
    full_name VARCHAR(100),
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    INDEX idx_username (username),
    INDEX idx_email (email)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- Todos table
CREATE TABLE IF NOT EXISTS todos (
    id INT AUTO_INCREMENT PRIMARY KEY,
    user_id INT NOT NULL,
    title VARCHAR(200) NOT NULL,
    description TEXT,
    status ENUM('pending', 'in_progress', 'completed', 'cancelled') DEFAULT 'pending',
    priority ENUM('low', 'medium', 'high', 'urgent') DEFAULT 'medium',
    due_date DATE,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,
    INDEX idx_user_id (user_id),
    INDEX idx_status (status),
    INDEX idx_priority (priority),
    INDEX idx_due_date (due_date)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- Insert sample data
INSERT INTO users (username, email, full_name) VALUES
    ('alice', 'alice@example.com', 'Alice Johnson'),
    ('bob', 'bob@example.com', 'Bob Smith'),
    ('charlie', 'charlie@example.com', 'Charlie Brown')
ON DUPLICATE KEY UPDATE username=username;

INSERT INTO todos (user_id, title, description, status, priority, due_date) VALUES
    (1, 'Complete project report', 'Write the final report for Q4 project', 'in_progress', 'high', DATE_ADD(CURDATE(), INTERVAL 3 DAY)),
    (1, 'Review pull requests', 'Review pending PRs on GitHub', 'pending', 'medium', DATE_ADD(CURDATE(), INTERVAL 1 DAY)),
    (2, 'Buy groceries', 'Milk, eggs, bread, vegetables', 'pending', 'low', CURDATE()),
    (2, 'Schedule dentist appointment', 'Need to book an appointment for next week', 'pending', 'medium', DATE_ADD(CURDATE(), INTERVAL 7 DAY)),
    (3, 'Prepare presentation', 'Create slides for team meeting', 'pending', 'high', DATE_ADD(CURDATE(), INTERVAL 2 DAY))
ON DUPLICATE KEY UPDATE id=id;


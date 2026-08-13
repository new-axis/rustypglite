use rustypglite::EmbeddedPg;

#[test]
fn test_start_and_stop() {
    let pg = EmbeddedPg::start().expect("Failed to start embedded postgres");

    let cs = pg.connection_string();
    assert!(!cs.is_empty(), "Connection string should not be empty");
    assert!(cs.contains("host="), "Should contain host");
    assert!(cs.contains("port="), "Should contain port");

    let port = pg.port();
    assert!(port > 0, "Port should be positive");

    let socket_dir = pg.socket_dir();
    assert!(!socket_dir.is_empty(), "Socket dir should not be empty");

    let data_dir = pg.data_dir();
    assert!(!data_dir.is_empty(), "Data dir should not be empty");
    assert!(
        std::path::Path::new(data_dir).join("PG_VERSION").exists(),
        "PG_VERSION should exist in data dir"
    );

    println!("Connection string: {}", cs);
    println!("Port: {}", port);
    println!("Socket dir: {}", socket_dir);

    // Server is running, socket should exist
    let socket_path = format!("{}/.s.PGSQL.{}", socket_dir, port);
    assert!(
        std::path::Path::new(&socket_path).exists(),
        "Unix socket file should exist at {}",
        socket_path
    );

    // Explicit stop
    pg.stop();
}

#[test]
fn test_exec_sql() {
    let pg = EmbeddedPg::start().expect("Failed to start");

    // Create a table and insert data via psql
    pg.exec_sql("CREATE TABLE test_items (id SERIAL PRIMARY KEY, name TEXT NOT NULL)")
        .expect("CREATE TABLE failed");

    pg.exec_sql("INSERT INTO test_items (name) VALUES ('Alice'), ('Bob')")
        .expect("INSERT failed");

    // Query via psql
    pg.exec_sql("SELECT COUNT(*) FROM test_items")
        .expect("SELECT failed");
}

#[test]
fn test_create_database() {
    let pg = EmbeddedPg::start().expect("Failed to start");

    pg.create_database("testdb").expect("CREATE DATABASE failed");

    // Execute SQL on the new database
    pg.exec_sql_on(Some("testdb"), "CREATE TABLE users (id SERIAL, email TEXT)")
        .expect("CREATE TABLE on testdb failed");

    pg.exec_sql_on(Some("testdb"), "INSERT INTO users (email) VALUES ('test@example.com')")
        .expect("INSERT on testdb failed");
}

#[test]
fn test_multiple_instances() {
    let pg1 = EmbeddedPg::start().expect("Failed to start instance 1");
    let pg2 = EmbeddedPg::start().expect("Failed to start instance 2");

    // Different ports
    assert_ne!(pg1.port(), pg2.port(), "Instances should have different ports");

    // Both should be able to run SQL independently
    pg1.exec_sql("CREATE TABLE t1 (id INT)").expect("pg1 CREATE failed");
    pg2.exec_sql("CREATE TABLE t2 (id INT)").expect("pg2 CREATE failed");

    // t1 should NOT exist on pg2
    let result = pg2.exec_sql("SELECT * FROM t1");
    assert!(result.is_err(), "t1 should not exist on pg2");
}

#[test]
fn test_builtin_functions() {
    let pg = EmbeddedPg::start().expect("Failed to start");

    // gen_random_uuid() is built-in since PG 13, no extension needed
    pg.exec_sql("SELECT gen_random_uuid()").expect("gen_random_uuid() failed");

    // JSON support
    pg.exec_sql("SELECT '{\"key\": \"value\"}'::jsonb -> 'key'")
        .expect("JSONB query failed");

    // CTEs
    pg.exec_sql("WITH cte AS (SELECT 1 AS n) SELECT n FROM cte")
        .expect("CTE query failed");
}

#[test]
fn test_connection_string_format() {
    let pg = EmbeddedPg::start().expect("Failed to start");

    let cs = pg.connection_string();
    // Should be semicolon-delimited key=value pairs (Npgsql format)
    assert!(cs.contains("host="));
    assert!(cs.contains("port="));
    assert!(cs.contains("database="));
    assert!(cs.contains("username=postgres"));
}

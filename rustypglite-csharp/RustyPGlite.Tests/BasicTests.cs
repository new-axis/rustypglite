using Xunit;
using Xunit.Abstractions;
using RustyPGlite;
using Npgsql;

namespace RustyPGlite.Tests;

public class BasicTests : IDisposable
{
    private readonly ITestOutputHelper _output;

    public BasicTests(ITestOutputHelper output)
    {
        _output = output;
    }

    public void Dispose() { }

    [Fact]
    public void CanStartAndStop()
    {
        using var pg = EmbeddedPg.Start();
        _output.WriteLine($"Connection string: {pg.ConnectionString}");
        _output.WriteLine($"Port: {pg.Port}");
        _output.WriteLine($"Socket: {pg.SocketDir}");
        Assert.NotEmpty(pg.ConnectionString);
        Assert.True(pg.Port > 0);
    }

    [Fact]
    public void CanConnectWithNpgsql()
    {
        using var pg = EmbeddedPg.Start();

        using var conn = new NpgsqlConnection(pg.ConnectionString);
        conn.Open();

        using var cmd = new NpgsqlCommand("SELECT 1 AS num", conn);
        var result = cmd.ExecuteScalar();
        Assert.Equal(1, result);
    }

    [Fact]
    public void CanCreateTablesAndCRUD()
    {
        using var pg = EmbeddedPg.Start();
        using var conn = new NpgsqlConnection(pg.ConnectionString);
        conn.Open();

        // CREATE TABLE
        using (var cmd = new NpgsqlCommand(@"
            CREATE TABLE users (
                id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
                email TEXT NOT NULL UNIQUE,
                name TEXT NOT NULL,
                metadata JSONB NOT NULL DEFAULT '{}',
                created_at TIMESTAMPTZ NOT NULL DEFAULT now()
            )", conn))
        {
            cmd.ExecuteNonQuery();
        }

        // INSERT
        using (var cmd = new NpgsqlCommand(
            "INSERT INTO users (email, name) VALUES (@email, @name) RETURNING id", conn))
        {
            cmd.Parameters.AddWithValue("email", "alice@example.com");
            cmd.Parameters.AddWithValue("name", "Alice");
            var id = cmd.ExecuteScalar();
            Assert.NotNull(id);
            _output.WriteLine($"Inserted user with id: {id}");
        }

        // SELECT
        using (var cmd = new NpgsqlCommand("SELECT email, name FROM users", conn))
        using (var reader = cmd.ExecuteReader())
        {
            Assert.True(reader.Read());
            Assert.Equal("alice@example.com", reader.GetString(0));
            Assert.Equal("Alice", reader.GetString(1));
        }

        // UPDATE
        using (var cmd = new NpgsqlCommand(
            "UPDATE users SET name = 'Alice Smith' WHERE email = @email", conn))
        {
            cmd.Parameters.AddWithValue("email", "alice@example.com");
            var rows = cmd.ExecuteNonQuery();
            Assert.Equal(1, rows);
        }

        // DELETE
        using (var cmd = new NpgsqlCommand("DELETE FROM users WHERE email = @email", conn))
        {
            cmd.Parameters.AddWithValue("email", "alice@example.com");
            var rows = cmd.ExecuteNonQuery();
            Assert.Equal(1, rows);
        }
    }

    [Fact]
    public void CanRunTransactions()
    {
        using var pg = EmbeddedPg.Start();
        using var conn = new NpgsqlConnection(pg.ConnectionString);
        conn.Open();

        new NpgsqlCommand("CREATE TABLE accounts (id INT PRIMARY KEY, balance INT)", conn).ExecuteNonQuery();
        new NpgsqlCommand("INSERT INTO accounts VALUES (1, 100), (2, 200)", conn).ExecuteNonQuery();

        // Transaction with rollback
        using (var tx = conn.BeginTransaction())
        {
            new NpgsqlCommand("UPDATE accounts SET balance = 0 WHERE id = 1", conn).ExecuteNonQuery();
            tx.Rollback();
        }

        // Balance should be unchanged
        using (var cmd = new NpgsqlCommand("SELECT balance FROM accounts WHERE id = 1", conn))
        {
            Assert.Equal(100, cmd.ExecuteScalar());
        }

        // Transaction with commit
        using (var tx = conn.BeginTransaction())
        {
            new NpgsqlCommand("UPDATE accounts SET balance = balance - 50 WHERE id = 1", conn).ExecuteNonQuery();
            new NpgsqlCommand("UPDATE accounts SET balance = balance + 50 WHERE id = 2", conn).ExecuteNonQuery();
            tx.Commit();
        }

        using (var cmd = new NpgsqlCommand("SELECT balance FROM accounts ORDER BY id", conn))
        using (var reader = cmd.ExecuteReader())
        {
            reader.Read();
            Assert.Equal(50, reader.GetInt32(0));
            reader.Read();
            Assert.Equal(250, reader.GetInt32(0));
        }
    }

    [Fact]
    public void CanUseJsonb()
    {
        using var pg = EmbeddedPg.Start();
        using var conn = new NpgsqlConnection(pg.ConnectionString);
        conn.Open();

        new NpgsqlCommand("CREATE TABLE docs (id SERIAL, data JSONB)", conn).ExecuteNonQuery();
        new NpgsqlCommand(
            "INSERT INTO docs (data) VALUES ('{\"name\": \"test\", \"tags\": [\"a\", \"b\"]}'::jsonb)",
            conn).ExecuteNonQuery();

        using var cmd = new NpgsqlCommand("SELECT data->>'name' FROM docs", conn);
        Assert.Equal("test", cmd.ExecuteScalar());
    }

    [Fact]
    public void CanCreateMultipleDatabases()
    {
        using var pg = EmbeddedPg.Start();

        pg.CreateDatabase("testdb1");
        pg.CreateDatabase("testdb2");

        // Connect to each and create independent tables
        using (var conn = new NpgsqlConnection(pg.ConnectionString.Replace("database=postgres", "database=testdb1")))
        {
            conn.Open();
            new NpgsqlCommand("CREATE TABLE t1 (id INT)", conn).ExecuteNonQuery();
        }

        using (var conn = new NpgsqlConnection(pg.ConnectionString.Replace("database=postgres", "database=testdb2")))
        {
            conn.Open();
            new NpgsqlCommand("CREATE TABLE t2 (id INT)", conn).ExecuteNonQuery();

            // t1 should not exist here
            Assert.Throws<PostgresException>(() =>
                new NpgsqlCommand("SELECT * FROM t1", conn).ExecuteNonQuery());
        }
    }

    [Fact]
    public void MultipleInstancesAreIsolated()
    {
        using var pg1 = EmbeddedPg.Start();
        using var pg2 = EmbeddedPg.Start();

        Assert.NotEqual(pg1.Port, pg2.Port);

        using var conn1 = new NpgsqlConnection(pg1.ConnectionString);
        using var conn2 = new NpgsqlConnection(pg2.ConnectionString);
        conn1.Open();
        conn2.Open();

        new NpgsqlCommand("CREATE TABLE shared_name (id INT)", conn1).ExecuteNonQuery();
        new NpgsqlCommand("CREATE TABLE shared_name (id INT)", conn2).ExecuteNonQuery();

        // Both succeed — fully isolated
        new NpgsqlCommand("INSERT INTO shared_name VALUES (1)", conn1).ExecuteNonQuery();
        new NpgsqlCommand("INSERT INTO shared_name VALUES (2)", conn2).ExecuteNonQuery();

        using var cmd1 = new NpgsqlCommand("SELECT id FROM shared_name", conn1);
        Assert.Equal(1, cmd1.ExecuteScalar());

        using var cmd2 = new NpgsqlCommand("SELECT id FROM shared_name", conn2);
        Assert.Equal(2, cmd2.ExecuteScalar());
    }
}

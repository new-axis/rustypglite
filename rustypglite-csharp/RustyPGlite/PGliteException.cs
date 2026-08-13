namespace RustyPGlite;

/// <summary>
/// Exception thrown by PGliteDatabase operations.
/// </summary>
public class PGliteException : Exception
{
    public PGliteException(string message) : base(message) { }
    public PGliteException(string message, Exception inner) : base(message, inner) { }
}

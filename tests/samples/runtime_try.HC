U0 Ping()
{
  "pong\n";
}

I64 Main()
{
  try {
    throw('E');
  } catch {
    "caught\n";
  }

  Ping;
  return 7;
}

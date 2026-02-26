I64 MightThrow(I64 code)
{
  if (code != 0) {
    throw(code);
  }
  return 0;
}

I64 Main()
{
  try {
    MightThrow(7);
  } catch {
    return 11;
  }
  return 1;
}

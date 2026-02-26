I64 Main()
{
  try {
    throw(1);
  } catch {
    return 2;
  }
  return 0;
}

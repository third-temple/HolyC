I64 Main()
{
  try {
    throw(1.5);
  } catch {
    return 0;
  }
  return 1;
}

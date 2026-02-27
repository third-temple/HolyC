U0 Worker(U8 *data)
{
}

I64 Main()
{
  CTask *task;
  task = Spawn(&Worker);
  if (task)
    return 1;
  return 0;
}

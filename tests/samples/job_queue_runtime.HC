I64 glbl;

U0 Worker(I64)
{
  lock glbl += 1;
}

I64 Main()
{
  CJob *j0, *j1;
  glbl = 0;
  j0 = JobQue(&Worker, 0, 0, 0);
  j1 = JobQue(&Worker, 0, 0, 0);
  JobResGet(j0);
  JobResGet(j1);
  return glbl;
}

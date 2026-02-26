I64 Main()
{
  I64 i=0;
  I64 acc=0;

  while (i<5) {
    if (i!=3)
      acc=acc+i;
    i=i+1;
  }

  for (i=0;i<3;i=i+1)
    acc=acc+2;

  do {
    acc=acc+1;
    break;
  } while (acc<100);

  return acc;
}

I64 Main()
{
  I64 x=0;
  for (x=0;x<3;x=x+1) {
    switch (x) {
      case 1:
        goto done;
      default:
        inner:
          x=x+1;
          break;
    }
  }

done:
  ;
  return x;
}

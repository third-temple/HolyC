U0 Main()
{
  I64 i;

  for (i = 1; i <= 100; i++) {
    if (i % 15 == 0)
      "FizzBuzz\n";
    else if (i % 3 == 0)
      "Fizz\n";
    else if (i % 5 == 0)
      "Buzz\n";
    else
      "%d\n", i;
  }
}
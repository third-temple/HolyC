class DemoMeta
{
  I64 age dft_val 7 print_str "%d";
};

I64 Main()
{
  CHashClass *klass = HashFind("DemoMeta", 0, 0);
  if (!klass)
    return 1;
  CMemberLst *ml = klass->member_lst_and_root;
  if (!ml)
    return 2;
  if (!MemberMetaFind("dft_val", ml))
    return 3;
  return MemberMetaData("dft_val", ml);
}

#include "protocol.hpp"

TEST_NOINLINE Word stack_arguments(Word a,Word b,Word c,Word d,Word e,
                                   Word f,Word g,Word h,Word i,Word j) {
    volatile Word local[]{a,b,c,d,e,f,g,h,i,j};
    Word sum=0;
    for (const auto value:local) sum+=value;
    return sum;
}
TEST_NOINLINE Word partial_store(Word input) {
    Word value=input;
    auto* bytes=reinterpret_cast<unsigned char*>(&value);
    bytes[1]=0xab; bytes[3]=0xcd;
    return value;
}
void test_body() {
    const Word input=test_input;
    record(0xfffffffdu+input);
    const Wide wide=(Wide(input)<<40)|0x12345678u;
    record(static_cast<Word>(wide>>32));
    record(static_cast<Word>((wide<<9)>>9));
    record((input*31)/3);
    record(partial_store(0x12345678));
    record(stack_arguments(input,2,3,4,5,6,7,8,9,10));
    Word count=0;
    for (Word n=0;n<input;++n) if (n%2) count+=n;
    record(count);
}

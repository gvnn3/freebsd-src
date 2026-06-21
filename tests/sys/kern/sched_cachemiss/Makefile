TESTSDIR=	${TESTSBASE}/sys/kern/sched_cachemiss

ATF_TESTS_C=	sched_cachemiss_test

TEST_METADATA.sched_cachemiss_test+=	is_exclusive="true"
TEST_METADATA.sched_cachemiss_test+=	timeout="120"

WARNS?=	3

.include <bsd.test.mk>

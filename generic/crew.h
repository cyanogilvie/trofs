/*
 * crew.h --
 *
 * Concurrent Read Exclusive Write
 *
 */

typedef struct CREW_Lock_ *CREW_Lock;

void CREW_ReadHold(CREW_Lock *clPtr);
void CREW_WriteHold(CREW_Lock *clPtr);
void CREW_Release(CREW_Lock *clPtr);
void CREW_Finalize(CREW_Lock *clPtr);


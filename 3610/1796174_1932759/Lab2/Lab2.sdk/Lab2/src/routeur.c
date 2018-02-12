#include "routeur.h"
#include "bsp_init.h"
#include "platform.h"
#include <stdlib.h>
#include <stdbool.h>
#include <xil_printf.h>
#include <xgpio.h>

///////////////////////////////////////////////////////////////////////////////////////
//								uC-OS global variables
///////////////////////////////////////////////////////////////////////////////////////
#define TASK_STK_SIZE 8192

///////////////////////////////////////////////////////////////////////////////////////
//								Routines d'interruptions
///////////////////////////////////////////////////////////////////////////////////////

void timer_isr(void* not_valid) {
	if (private_timer_irq_triggered()) {
		private_timer_clear_irq();
		OSTimeTick();
	}
}

void fit_timer_1s_isr(void *not_valid) {
	uint8_t err;
	err = OSSemPost(semVerifySrc);
	err_msg("semVerifySrc_fit_timer_1s_isr", err);
}

void fit_timer_3s_isr(void *not_valid) {
	uint8_t err;
	err = OSSemPost(semVerifyCRC);
	err_msg("semVerifyCRC_fit_timer_3s_isr", err);
}

void gpio_isr(void * not_valid) {
	OSSemPost(semReset);
	XGpio_InterruptClear(&gpSwitch, 0xFFFFFFFF);
}

/*
 *********************************************************************************************************
 *                                            computeCRC
 * -Calcule la check value d'un pointeur quelconque (cyclic redudancy check)
 * -Retourne 0 si le CRC est correct, une autre valeur sinon.
 *********************************************************************************************************
 */
unsigned int computeCRC(uint16_t* w, int nleft) {
	unsigned int sum = 0;
	uint16_t answer = 0;

	// Adding words of 16 bits
	while (nleft > 1) {
		sum += *w++;
		nleft -= 2;
	}

	// Handling the last byte
	if (nleft == 1) {
		*(unsigned char *) (&answer) = *(const unsigned char *) w;
		sum += answer;
	}

	// Handling overflow
	sum = (sum & 0xffff) + (sum >> 16);
	sum += (sum >> 16);

	answer = ~sum;
	return (unsigned int) answer;
}

/*
 *********************************************************************************************************
 *                                          computePacketCRC
 * -Calcule la check value d'un paquet en utilisant un CRC (cyclic redudancy check)
 * -Retourne 0 si le CRC est correct, une autre valeur sinon.
 *********************************************************************************************************
 */
static inline unsigned int computePacketCRC(Packet* packet) {
	return computeCRC((uint16_t*) packet, sizeof(Packet));
}
///////////////////////////////////////////////////////////////////////////////////////
//								uC/OS-II part
///////////////////////////////////////////////////////////////////////////////////////
int main() {

	initialize_bsp();

	// Initialize uC/OS-II
	OSInit();

	create_application();

	prepare_and_enable_irq();

	xil_printf("*** Starting uC/OS-II scheduler ***\n");

	OSStart();

	mode_profilage = 0;

	cleanup();
	cleanup_platform();

	return 0;
}

void create_application() {
	int error;

	error = create_tasks();
	if (error != 0)
		xil_printf("Error %d while creating tasks\n", error);

	error = create_events();
	if (error != 0)
		xil_printf("Error %d while creating events\n", error);
}

int create_tasks() {
	// Stacks
	static OS_STK TaskReceiveStk[TASK_STK_SIZE];
	static OS_STK TaskVerifySourceStk[TASK_STK_SIZE];
	static OS_STK TaskVerifyCRCStk[TASK_STK_SIZE];
	static OS_STK TaskStatsStk[TASK_STK_SIZE];
	static OS_STK TaskComputeStk[TASK_STK_SIZE];
	static OS_STK TaskForwardingStk[TASK_STK_SIZE];
	static OS_STK TaskPrint1Stk[TASK_STK_SIZE];
	static OS_STK TaskPrint2Stk[TASK_STK_SIZE];
	static OS_STK TaskPrint3Stk[TASK_STK_SIZE];

	OSTaskCreate(TaskGeneratePacket, NULL, &TaskReceiveStk[TASK_STK_SIZE-1], TASK_GENERATE_PRIO);
	OSTaskCreate(TaskVerifySource, NULL, &TaskVerifySourceStk[TASK_STK_SIZE-1], TASK_STOP_PRIO);
	OSTaskCreate(TaskVerifyCRC, NULL, &TaskVerifyCRCStk[TASK_STK_SIZE-1], TASK_RESET_PRIO);
	OSTaskCreate(TaskStats, NULL, &TaskStatsStk[TASK_STK_SIZE-1], TASK_STATS_PRIO);
	OSTaskCreate(TaskComputing, NULL, &TaskComputeStk[TASK_STK_SIZE-1], TASK_COMPUTING_PRIO);
	OSTaskCreate(TaskForwarding, NULL, &TaskForwardingStk[TASK_STK_SIZE-1], TASK_FORWARDING_PRIO);
	OSTaskCreate(TaskPrint, &print_param[0], &TaskPrint1Stk[TASK_STK_SIZE-1], TASK_PRINT1_PRIO);
	OSTaskCreate(TaskPrint, &print_param[1], &TaskPrint2Stk[TASK_STK_SIZE-1], TASK_PRINT2_PRIO);
	OSTaskCreate(TaskPrint, &print_param[2], &TaskPrint3Stk[TASK_STK_SIZE-1], TASK_PRINT3_PRIO);
	
	return 0;
}

int create_events() {
	uint8_t err;

	static void* inputMsg[1024];
	static void* lowMsg[1024];
	static void* mediumMsg[1024];
	static void* highMsg[1024];

	inputQ = OSQCreate(&inputMsg[0], 1024);
	lowQ = OSQCreate(&lowMsg[0], 1024);
	mediumQ = OSQCreate(&mediumMsg[0], 1024);
	highQ = OSQCreate(&highMsg[0], 1024);

	semVerifySrc = OSSemCreate(0);
	semVerifyCRC = OSSemCreate(0);
	semStats = OSSemCreate(0);

	mutexPacketSourceRejete = OSMutexCreate(MUT_REJET_PRIO, &err);
	mutexPacketCRCRejete = OSMutexCreate(MUT_CRC_PRIO, &err);
	mutexPrinting = OSMutexCreate(MUT_PRINT_PRIO, &err);
	mutexMemory = OSMutexCreate(MUT_MALLOC_PRIO, &err);
	mutexPacketTraites = OSMutexCreate(MUT_TRAITES_PRIO,&err);

	return 0;
}

///////////////////////////////////////////////////////////////////////////////////////
//								uC/OS-II part
///////////////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////////////
//									TASKS
///////////////////////////////////////////////////////////////////////////////////////

/*
 *********************************************************************************************************
 *											  TaskGeneratePacket
 *  - Génère des paquets et les envoie dans la InputQ.
 *  - À des fins de développement de votre application, vous pouvez *temporairement* modifier la variable
 *    "shouldSlowthingsDown" à  true pour ne générer que quelques paquets par seconde, et ainsi pouvoir
 *    déboguer le flot de vos paquets de manière plus saine d'esprit. Cependant, la correction sera effectuée
 *    avec cette variable Ã  false.
 *********************************************************************************************************
 */
void TaskGeneratePacket(void *data) {
	srand(42);
	uint8_t err;
	bool isGenPhase = true; 					// Indique si on est dans la phase de generation ou non
	const bool shouldSlowThingsDown = true;		// Variable à modifier
	int packGenQty = (rand() % 250);
	while (true) {
		if (isGenPhase) {
			Packet *packet = malloc(sizeof(Packet));

			packet->src = rand() * (UINT32_MAX / RAND_MAX);
			packet->dst = rand() * (UINT32_MAX / RAND_MAX);
			packet->type = rand() % NB_PACKET_TYPE;

			//Compute CRC
			packet->crc = 0;
			if (rand() % 10 == 9) // 10% of Packets with bad CRC
				packet->crc = 1234;
			else
				packet->crc = computeCRC((unsigned short*) (&packet), 64);

			for (int i = 0; i < ARRAY_SIZE(packet->data); ++i)
				packet->data[i] = (unsigned int) rand();
			packet->data[0] = nbPacketCrees;

			nbPacketCrees++;

			if (shouldSlowThingsDown) {
				xil_printf("GENERATE : ********GÃ©nÃ©ration du Paquet # %d ******** \n", nbPacketCrees);
				xil_printf("ADD %x \n", packet);
				xil_printf("	** src : %x \n", packet->src);
				xil_printf("	** dst : %x \n", packet->dst);
				xil_printf("	** crc : %x \n", packet->crc);
				xil_printf("	** type : %d \n", packet->type);
			}

			err = OSQPost(inputQ, packet);

			if (err == OS_ERR_Q_FULL) {
				xil_printf(
						"GENERATE: Paquet rejeté a l'entrée car la FIFO est pleine !\n");
				free(packet);
			}

			if (shouldSlowThingsDown) {
				OSTimeDlyHMSM(0, 0, 0, 200 + rand() % 600);
			} else {
				OSTimeDlyHMSM(0, 0, 0, 2);

				if ((nbPacketCrees % packGenQty) == 0) //On génère jusqu'à 250 paquets par phase de génération
						{
					isGenPhase = false;
				}
			}
		} else {
			OSTimeDlyHMSM(0, 0, 0, 500);
			isGenPhase = true;
			packGenQty = (rand() % 250);
			xil_printf(
					"GENERATE: Génération de %d paquets durant les %d prochaines millisecondes\n",
					packGenQty, packGenQty * 2);
		}
	}
}

/*
 *********************************************************************************************************
 *											  TaskVerifySource
 *  -Stoppe le routeur une fois que 200 paquets ont été rejetés pour mauvaise source
 *  -Ne doit pas stopper la tâche d'affichage des statistiques.
 *********************************************************************************************************
 */
void TaskVerifySource(void *data) {
	uint8_t err;
	while(true) {
		OSSemPend(semVerifySrc, 0, &err);

		err_msg("semVerifySrc", err);
		OSMutexPend(mutexPacketSourceRejete, 0, &err);
		err_msg("Pend mutexPacketSourceRejete", err);
		if (nbPacketSourceRejete >= 200) {
			OSTaskSuspend(TASK_GENERATE_PRIO);
			OSTaskSuspend(TASK_COMPUTING_PRIO);
			OSTaskSuspend(TASK_FORWARDING_PRIO);
			OSTaskSuspend(TASK_PRINT1_PRIO);
			OSTaskSuspend(TASK_PRINT2_PRIO);
			OSTaskSuspend(TASK_PRINT3_PRIO);
			
		}
		err = OSMutexPost(mutexPacketSourceRejete);
		err_msg("Post mutexPacketSourceRejete", err);

	}
}

/*
 *********************************************************************************************************
 *											  TaskVerifyCRC
 *  -Stoppe le routeur une fois que 200 paquets ont été rejetés pour mauvais CRC
 *  -Ne doit pas stopper la tâche d'affichage des statistiques.
 *********************************************************************************************************
 */
void TaskVerifyCRC(void *data) {
	uint8_t err;
	while(true) {
		OSSemPend(semVerifyCRC, 0, &err);

		err_msg("semVerifyCRC", err);
		OSMutexPend(mutexPacketCRCRejete, 0, &err);
		err_msg("Pend mutexPacketCRCRejete", err);
		if (nbPacketCRCRejete >= 200) {
			OSTaskSuspend(TASK_GENERATE_PRIO);
			OSTaskSuspend(TASK_COMPUTING_PRIO);
			OSTaskSuspend(TASK_FORWARDING_PRIO);
			OSTaskSuspend(TASK_PRINT1_PRIO);
			OSTaskSuspend(TASK_PRINT2_PRIO);
			OSTaskSuspend(TASK_PRINT3_PRIO);
			
		}
		err = OSMutexPost(mutexPacketCRCRejete);
		err_msg("Post mutexPacketCRCRejete", err);

	}
}

/*
 *********************************************************************************************************
 *											  TaskComputing
 *  -Vérifie si les paquets sont conformes (CRC,Adresse Source)
 *  -Dispatche les paquets dans des files (HIGH,MEDIUM,LOW)
 *
 *********************************************************************************************************
 */
void TaskComputing(void *pdata) {
	uint8_t err;
	Packet *packet = NULL;
	int waitCnt = 220000;
	while(true){
		packet = OSQPend(inputQ, 0, &err);
		err_msg("inputQ", err);

		while (--waitCnt);
		waitCnt = 220000;

		if ((packet->src >= REJECT_LOW1 && packet->src <= REJECT_HIGH1)|
			(packet->src >= REJECT_LOW2 && packet->src <= REJECT_HIGH2)|
			(packet->src >= REJECT_LOW3 && packet->src <= REJECT_HIGH3)|
			(packet->src >= REJECT_LOW4 && packet->src <= REJECT_HIGH4)){

			OSMutexPend(mutexPacketSourceRejete, 0, &err);
			err_msg("Pend mutexPacketSourceRejete", err);
			nbPacketSourceRejete++;
			free(packet);
			err = OSMutexPost(mutexPacketSourceRejete);
			err_msg("Post mutexPacketSourceRejete", err);
			
		}
		else if (computePacketCRC(packet)!=0){
			OSMutexPend(mutexPacketCRCRejete, 0, &err);
			err_msg("Pend mutexPacketCRCRejete", err);
			nbPacketCRCRejete++;
			free(packet);
			err = OSMutexPost(mutexPacketCRCRejete);
			err_msg("Post mutexPacketCRCRejete", err);
		}
		else if (packet->type == PACKET_VIDEO) {
			err = OSQPost(highQ, packet);
			if (err == OS_ERR_Q_FULL){
				OSMutexPend(mutexMemory, 0, &err);
				err_msg("Pend mutexMemory", err);
				free(packet);
				err = OSMutexPost(mutexMemory);
				err_msg("Post mutexMemory", err);
				err = OSMutexPost(mutexPrinting);
				err_msg("Post mutexPrinting", err);
				xil_printf("Packet_Video rejete, queue full\n");
				err = OSMutexPost(mutexPrinting);
				err_msg("Post mutexPrinting", err);

			}
			else if (err != OS_NO_ERR) {
				err_msg("highQ", err);
			}
			else {
				OSMutexPend(mutexPacketTraites, 0, &err);
				err_msg("Pend mutexPacketTraites", err);
				nbPacketTraites++;
				err = OSMutexPost(mutexPacketTraites);
				err_msg("Post mutexPacketTraites", err);
			}
		}
		else if (packet->type == PACKET_AUDIO) {
			err = OSQPost(mediumQ, packet);
			if (err == OS_ERR_Q_FULL){
				OSMutexPend(mutexMemory, 0, &err);
				err_msg("Pend mutexMemory", err);
				free(packet);
				err = OSMutexPost(mutexMemory);
				err_msg("Post mutexMemory", err);
				err = OSMutexPost(mutexPrinting);
				err_msg("Post mutexPrinting", err);
				xil_printf("Packet audio rejete, queue full\n");
				err = OSMutexPost(mutexPrinting);
				err_msg("Post mutexPrinting", err);
			}
			else if (err != OS_NO_ERR) {
				err_msg("mediumQ", err);
			}
			else {
				OSMutexPend(mutexPacketTraites, 0, &err);
				err_msg("Pend mutexPacketTraites", err);
				nbPacketTraites++;
				err = OSMutexPost(mutexPacketTraites);
				err_msg("Post mutexPacketTraites", err);
			}
		}
		else if (packet->type == PACKET_AUTRE) {
			err = OSQPost(lowQ, packet);
			if (err == OS_ERR_Q_FULL){
				OSMutexPend(mutexMemory, 0, &err);
				err_msg("Pend mutexMemory", err);
				free(packet);
				err = OSMutexPost(mutexMemory);
				err_msg("Post mutexMemory", err);
				err = OSMutexPost(mutexPrinting);
				err_msg("Post mutexPrinting", err);
				xil_printf("Packet autre rejete queue full\n");
				err = OSMutexPost(mutexPrinting);
				err_msg("Post mutexPrinting", err);
			}
			else if (err != OS_NO_ERR) {
				err_msg("lowQ", err);
			}
			else {
				OSMutexPend(mutexPacketTraites, 0, &err);
				err_msg("Pend mutexPacketTraites", err);
				nbPacketTraites++;
				err = OSMutexPost(mutexPacketTraites);
				err_msg("Post mutexPacketTraites", err);
			}
		}
		else {
			err = OSMutexPost(mutexPrinting);
			err_msg("Post mutexPrinting", err);
			xil_printf("WARNING: Unknown packet type!\n");
			err = OSMutexPost(mutexPrinting);
			err_msg("Post mutexPrinting", err);
		}

	}
}
/*
 *********************************************************************************************************
 *											  TaskForwarding
 *  -Traite la priorité des paquets : si un paquet de haute priorité est prêt,
 *   on l'envoie à l'aide de la fonction dispatch, sinon on regarde les paquets de moins haute priorité
 *********************************************************************************************************
 */
void TaskForwarding(void *pdata) {
	uint8_t err;
	Packet *packet = NULL;
	while (true) {
		/* À compléter */
		packet = OSQAccept(highQ, &err);
		err_msg("Error accepting queue", err);
		if (packet == NULL) {
			packet = OSQAccept(mediumQ, &err);
			err_msg("Error accepting queue", err);
		}
		if (packet == NULL) {
			packet = OSQAccept(lowQ, &err);
			err_msg("Error accepting queue", err);
		}
		else if (packet != NULL) {
			if (packet->dst >= INT1_LOW && packet->dst <= INT1_HIGH) {
				err = OSMboxPost(mbox[0], packet);
				err_msg("Error posting mbox", err);
			}
			else if (packet->dst >= INT2_LOW && packet->dst <= INT2_HIGH) {
				err = OSMboxPost(mbox[1], packet);
				err_msg("Error posting mbox", err);
			}
			else if (packet->dst >= INT3_LOW && packet->dst <= INT3_HIGH) {
				err = OSMboxPost(mbox[2], packet);
				err_msg("Error posting mbox", err);
			}
			else if (packet->dst >= INT_BC_LOW && packet->dst <= INT_BC_HIGH) {
				OSMutexPend(mutexMemory, 0, &err);
				err_msg("Error accepting mutex", err);
				Packet *packet2 = malloc(sizeof(Packet));
				*packet2 = *packet;
				Packet *packet3 = malloc(sizeof(Packet));
				*packet3 = *packet;
				err = OSMutexPost(mutexMemory);
				err_msg("Error posting mutex", err);
				
				err = OSMboxPost(mbox[0], packet);
				err_msg("Error posting mbox", err);
				err = OSMboxPost(mbox[1], packet2);
				err_msg("Error posting mbox", err);
				err = OSMboxPost(mbox[2], packet3);
				err_msg("Error posting mbox", err);
			}
		}
	}
}

/*
 *********************************************************************************************************
 *                                              TaskStats
 *  -Est déclenchée lorsque le gpio_isr() libère le sémpahore
 *  -Si en mode profilage, calcule les statistiques des files
 *  -En sortant de la période de profilage, affiche les statistiques des files et du routeur.
 *********************************************************************************************************
 */
void TaskStats(void *pdata) {
	uint8_t err;
	while (true) {
		/* À compléter */

		xil_printf("\n------------------ Affichage des statistiques ------------------\n");
		xil_printf("Nb de packets total traites : %d\n", nbPacketCrees);
		xil_printf("Nb de packets total traites : %d\n", nbPacketTraites);
		xil_printf("Nb de packets rejetes pour mauvaise source : %d\n",	nbPacketSourceRejete);
		xil_printf("Nb de packets rejetes pour mauvais crc : %d\n",	nbPacketCRCRejete);

		xil_printf("Nb d'echantillons de la période de profilage : %d\n", nb_echantillons);
		xil_printf("Maximum file input : %d\n", max_msg_input);
		xil_printf("Moyenne file input : %d\n", moyenne_msg_input);
		xil_printf("Maximum file low : %d\n", max_msg_low);
		xil_printf("Moyenne file low : %d\n", moyenne_msg_low);
		xil_printf("Maximum file medium : %d\n", max_msg_medium);
		xil_printf("Moyenne file medium : %d\n", moyenne_msg_medium);
		xil_printf("Maximum file high : %d\n", max_msg_high);
		xil_printf("Moyenne file high : %d\n", moyenne_msg_high);

		/* À compléter */
	}
}

/*
 *********************************************************************************************************
 *											  TaskPrint
 *  -Affiche les infos des paquets arrivés Ã  destination et libere la mémoire allouée
 *********************************************************************************************************
 */
void TaskPrint(void *data) {
	uint8_t err;
	Packet *packet = NULL;
	int intID = ((PRINT_PARAM*)data)->interfaceID;
	OS_EVENT* mb = ((PRINT_PARAM*)data)->Mbox;

	while(true){
		packet = OSMboxPend(mb, 0, &err);
		err_msg("OSMboxPend TaskPrint", err);

		if (packet != NULL) {
			OSMutexPend(mutexPrinting, 0, &err);
			err_msg("mutexPrinting", err);
			xil_printf("INT %d - SRC %08x - DST %08x - TYPE %d - CRC  %d - DATA %x\n", intID, packet->src, packet->dst, packet->type, packet->crc, packet->data);
			err = OSMutexPost(mutexPrinting);
			err_msg("mutexPrinting", err);

			OSMutexPend(mutexMemory, 0, &err);
			err_msg("mutexMemory", err);
			free(packet);
			err = OSMutexPost(mutexMemory);
			err_msg("mutexMemory", err);
		}

	}

}

void err_msg(char* entete, uint8_t err) {
	if (err != 0) {
		xil_printf(entete);
		xil_printf(": Une erreur est retournée : code %d \n", err);
	}
}

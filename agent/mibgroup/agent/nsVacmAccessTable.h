/*
 * Note: this file originally auto-generated by mib2c using
 *  : mib2c.iterate.conf,v 5.17 2005/05/09 08:13:45 dts12 Exp $
 */
#ifndef NSVACMACCESSTABLE_H
#define NSVACMACCESSTABLE_H

config_add_mib(NET-SNMP-VACM-MIB);

/*
 * function declarations 
 */
void            init_nsVacmAccessTable(void);
void            initialize_table_nsVacmAccessTable(void);
void            init_register_nsVacm_context(const char *context);
Netsnmp_Node_Handler nsVacmAccessTable_handler;
Netsnmp_First_Data_Point nsVacmAccessTable_get_first_data_point;
Netsnmp_Next_Data_Point nsVacmAccessTable_get_next_data_point;

/*
 * column number definitions for table nsVacmAccessTable 
 */
#define COLUMN_NSVACMTOKEN		1
#define COLUMN_NSVACMCONTEXTMATCH	2
#define COLUMN_NSVACMVIEWNAME		3
#define COLUMN_VACMACCESSSTORAGETYPE	4
#define COLUMN_NSVACMACCESSSTATUS	5
#endif                          /* NSVACMACCESSTABLE_H */

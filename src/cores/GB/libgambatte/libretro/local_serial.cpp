#include "local_serial.h"

unsigned char LocalSerial::send(unsigned char data, bool fastCgb)
{	
	unsigned char data_in = this->connected_SerialIO->receive(data, fastCgb);
	log_link_traffic(data, data_in);
	return data_in;
	
}

unsigned char LocalSerial::receive(unsigned char data, bool fastCgb)
{
	has_received_data = true;
	received_data = data;
	this->fastCgb = fastCgb;
	return link_target->receive_from_linkcable(data);
}

bool LocalSerial::check(unsigned char out, unsigned char& in, bool& fastCgb)
{
	//return false;
	
	if (has_received_data) 
	{
		in = received_data;
		fastCgb = this->fastCgb;
		has_received_data = false; 
		return true;
	}

	return false; 
}


void LocalSerial::log_link_traffic(unsigned char a, unsigned char b)
{

		std::string filePath = "./2p_link_log.txt";
		std::ofstream ofs(filePath.c_str(), std::ios_base::out | std::ios_base::app);

		//int clocks_occer = total_clock - clocks_since_last_serial;
		//std::string tabs = clocks_occer < 1000000 ? "\t\t\t" : "\t\t";

		//ofs << "" << clocks_occer << tabs;
		ofs << "" << std::hex << (int)a << "\t";
		ofs << "" << std::hex << (int)b << "";

		ofs << std::endl;
		ofs.close();

		//clocks_since_last_serial = total_clock;
	

}
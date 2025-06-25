//#include "linkcable.h"
#include <gambatte.h>
#include <string>
#include <istream>
#include <iostream>
#include <fstream>

class LocalSerial : public gambatte::SerialIO
{
public:
	LocalSerial() {};
	~LocalSerial() {};

	void setLinkTarget(I_linkcable_target* link_target) { this->link_target = link_target; };
	void setConnectedSerialIO(LocalSerial* serialIO) { this->connected_SerialIO = serialIO; };

	virtual bool check(unsigned char out, unsigned char& in, bool& fastCgb);
	virtual unsigned char send(unsigned char data, bool fastCgb);
	bool is_ready() { return link_target->is_ready(); };
	virtual unsigned char receive(unsigned char data, bool fastCgb);

private:

	void log_link_traffic(unsigned char a, unsigned char b);

	I_linkcable_target* link_target;
	LocalSerial* connected_SerialIO;

	bool has_received_data, fastCgb;
	unsigned char received_data, out_data;
};
class I_linkcable_target {

public:
	virtual unsigned char receive_from_linkcable(unsigned char data) = 0;
	bool is_ready() { return true; };
	
};

class I_linkcable_sender {
	friend class gb;
public:
	virtual unsigned char send_over_linkcable(unsigned char data) = 0;
};
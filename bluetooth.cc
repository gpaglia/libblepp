
/*
   Copyright (c) 2013  Edward Rosten

	All rights reserved.

	Redistribution and use in source and binary forms, with or without
	modification, are permitted provided that the following conditions are met:
		* Redistributions of source code must retain the above copyright
		  notice, this list of conditions and the following disclaimer.
		* Redistributions in binary form must reproduce the above copyright
		  notice, this list of conditions and the following disclaimer in the
		  documentation and/or other materials provided with the distribution.
		* Neither the name of the <organization> nor the
		  names of its contributors may be used to endorse or promote products
		  derived from this software without specific prior written permission.

	THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
	ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
	WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
	DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
	DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
	(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
	LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
	ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
	(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
	SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


#include <iostream>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <vector>
#include <sstream>
#include <iomanip>
#include <cassert>
#include <tuple>
#include <stdexcept>
#include <functional>
#include <algorithm>

#include  <libattgatt/logging.h>
#include  <libattgatt/bledevice.h>
#include "lib/uuid.h"
#include <libattgatt/att_pdu.h>
#include <libattgatt/pretty_printers.h>
using namespace std;



//Easier to use implementation of the ATT protocol.
//This implementation reads whole chunks of stuff in one go
//and feeds back the data to the user.
struct SimpleBlockingATTDevice: public BLEDevice
{
	SimpleBlockingATTDevice(const std::string& s)
	:BLEDevice(s)
	{}

	template<class Ret, class PDUType, class E, class F, class G> 
	vector<Ret> read_multiple(int request, int response, const E& call,  const F& func, const G& last)
	{
		vector<Ret> ret;
		vector<uint8_t> buf(ATT_DEFAULT_MTU);
		
		int start=1;

		for(;;)
		{
			call(start, 0xffff);
			PDUResponse r = receive(buf);
		
			if(r.type() == ATT_OP_ERROR)
			{
				PDUErrorResponse err = r;

				if(err.request_opcode() != request)
				{
					LOG(Error, string("Unexpected opcode in error. Expected ") + att_op2str(request) + " got "  + att_op2str(err.request_opcode()));
					throw 1;
				}
				else if(err.error_code() != ATT_ECODE_ATTR_NOT_FOUND)
				{
					LOG(Error, string("Received unexpected error:") + att_ecode2str(err.error_code()));
					throw 1;
				}
				else 
					break;
			}
			else if(r.type() != response)
			{
					LOG(Error, string("Unexpected response. Expected ") + att_op2str(response) + " got "  + att_op2str(r.type()));
					//FIXME
					//Break if the packed is NOT a notification or indication
			}
			else
			{
				PDUType t = r;
				for(int i=0; i < t.num_elements(); i++)
					ret.push_back(func(t, i));
				
				if(last(t) == 0xffff)
					break;
				else
					start = last(t)+1;

				LOG(Debug, "New start = " << start);
			}
		}

		return ret;

	}


	vector<pair<uint16_t, vector<uint8_t>>> read_by_type(const bt_uuid_t& uuid)
	{
		return read_multiple<pair<uint16_t, vector<uint8_t>>, PDUReadByTypeResponse>(ATT_OP_READ_BY_TYPE_REQ, ATT_OP_READ_BY_TYPE_RESP, 
			[&](int start, int end)
			{
				send_read_by_type(uuid, start, end);	
			},
			[](const PDUReadByTypeResponse& p, int i)
			{
				return make_pair(p.handle(i),  vector<uint8_t>(p.value(i).first, p.value(i).second));
			}, 
			[](const PDUReadByTypeResponse& p)
			{
				return p.handle(p.num_elements()-1);
			})
			;

	}
	
	
	vector<pair<uint16_t, bt_uuid_t>> find_information()
	{
		return read_multiple<pair<uint16_t, bt_uuid_t>, PDUFindInformationResponse>(ATT_OP_FIND_INFO_REQ, ATT_OP_FIND_INFO_RESP, 
			[&](int start, int end)
			{
				send_find_information(start, end);
			},
			[](const PDUFindInformationResponse&p, int i)
			{
				return make_pair(p.handle(i), p.uuid(i));
			},
			[](const PDUFindInformationResponse& p)
			{
				return p.handle(p.num_elements()-1);
			});
	}
};


///Interpret a ReadByTypeResponse packet as a ReadCharacteristic packet
class GATTReadCharacteristic: public  PDUReadByTypeResponse
{
	public:
	struct Characteristic
	{
		uint16_t handle;
		uint8_t  flags;
		bt_uuid_t uuid;
	};

	GATTReadCharacteristic(const PDUResponse& p)
	:PDUReadByTypeResponse(p)
	{
		if(value_size() != 5 && value_size() != 19)		
			throw runtime_error("Invalid packet size in GATTReadCharacteristic");
	}

	Characteristic characteristic(int i) const
	{
		Characteristic c;
		c.handle = att_get_u16(value(i).first + 1);
		c.flags  = value(i).first[0];

		if(value_size() == 5)
			c.uuid= att_get_uuid16(value(i).first + 3);
		else
			c.uuid= att_get_uuid128(value(i).first + 3);

		return c;
	}
};



///Interpret a ReadByTypeResponse packet as a ReadCharacteristic packet
class GATTReadCCC: public  PDUReadByTypeResponse
{
	public:

	GATTReadCCC(const PDUResponse& p)
	:PDUReadByTypeResponse(p)
	{
		if(value_size() != 2)
			throw runtime_error("Invalid packet size in GATTReadCharacteristic");
	}

	uint16_t ccc(int i) const
	{
		return att_get_u16(value(i).first);
	}
};
///Interpret a read_group_by_type resoponde as a read service group response
class GATTReadServiceGroup: public PDUReadGroupByTypeResponse
{
	public:
	GATTReadServiceGroup(const PDUResponse& p)
	:PDUReadGroupByTypeResponse(p)
	{
		if(value_size() != 2 && value_size() != 16)
		{
			LOG(Error, "UUID length" << value_size());
			error<std::runtime_error>("Invalid UUID length in PDUReadGroupByTypeResponse");
		}
	}

	bt_uuid_t uuid(int i) const
	{
		const uint8_t* begin = data + i*element_size() + 6;

		bt_uuid_t uuid;
		if(value_size() == 2)
		{
			uuid.type = BT_UUID16;
			uuid.value.u16 = att_get_u16(begin);
		}
		else
		{
			uuid.type = BT_UUID128;
			uuid.value.u128 = att_get_u128(begin);
		}
			
		return uuid;
	}
};


//This class layers the GATT profile on top of the ATT proticol
//Provides higher level GATT specific meaning to attributes.
class SimpleBlockingGATTDevice: public SimpleBlockingATTDevice
{
	public:
	SimpleBlockingGATTDevice(const std::string& s)
	:SimpleBlockingATTDevice(s)
	{
	}

	typedef GATTReadCharacteristic::Characteristic Characteristic;

	vector<pair<uint16_t, Characteristic>> read_characaristic()
	{
		bt_uuid_t uuid;
		uuid.value.u16 = GATT_CHARACTERISTIC;
		uuid.type = BT_UUID16;
		return read_multiple<pair<uint16_t, Characteristic>, GATTReadCharacteristic>(ATT_OP_READ_BY_TYPE_REQ, ATT_OP_READ_BY_TYPE_RESP, 
			[&](int start, int end)
			{
				send_read_by_type(uuid, start, end);	
			},
			[](const GATTReadCharacteristic& p, int i)
			{
				return make_pair(p.handle(i), p.characteristic(i));
			},
			[](const GATTReadCharacteristic& p)
			{
				return p.handle(p.num_elements()-1);
			});
	}

	vector<tuple<uint16_t, uint16_t, bt_uuid_t>> read_service_group(const bt_uuid_t& uuid)
	{
		return read_multiple<tuple<uint16_t, uint16_t, bt_uuid_t>, GATTReadServiceGroup>(ATT_OP_READ_BY_GROUP_REQ, ATT_OP_READ_BY_GROUP_RESP, 
			[&](int start, int end)
			{
				send_read_group_by_type(uuid, start, end);	
			},
			[](const GATTReadServiceGroup& p, int i)
			{
				return make_tuple(p.start_handle(i),  p.end_handle(i), p.uuid(i));
			},
			[](const GATTReadServiceGroup& p)
			{
				return p.end_handle(p.num_elements()-1);
			});
	}

};






template<class C> const C& haxx(const C& X)
{
	return X;
}

int haxx(uint8_t X)
{
	return X;
}
//#define LOGVAR(X) LOG(Debug, #X << " = " << haxx(X))
#define LOGVAR(X) cerr << #X << " = " << haxx(X) << endl

class BLEGATTStateMachine;

enum  States
{
	Idle,
	ReadingPrimaryService,
	FindAllCharacteristics,
	GetClientCharaceristicConfiguration,
	AwaitingWriteResponse,
};

static const int Waiting=-1;


class UUID: public bt_uuid_t
{
	public:

	explicit UUID(const uint16_t& u)
	{
		type = BT_UUID16;
		value.u16 = u;
	}
	
	UUID(){}

	UUID(const UUID&) = default;

	static UUID from(const bt_uuid_t& uuid)
	{
		UUID ret;
		(bt_uuid_t&)ret = uuid;

		return ret;
	}

	bool operator==(const UUID& uuid) const
	{
		return bt_uuid_cmp(this, &uuid) == 0;
	}
};


struct Characteristic
{	
	private:
	BLEGATTStateMachine* s;

	public:

	Characteristic(BLEGATTStateMachine* s_)
	:s(s_)
	{}

	void set_notify_and_indicate(bool , bool);
	std::function<void(const PDUNotificationOrIndication&)> cb_notify_or_indicate;

	//Flags indicating various properties
	bool broadcast, read, write_without_response, write, notify, indicate, authenticated_write, extended;

	//UUID, i.e. name of what the characteristic represents semantically
	UUID uuid;

	//Where the value can be read/written
	uint16_t value_handle;

	//Where we write to configure, i.e. set notify and/or indicate
	//0 means invalid.
	uint16_t client_characteric_configuration_handle;
	uint16_t ccc_last_known_value;
	
	uint16_t first_handle, last_handle;
};

struct StateMachineGoneBad: public runtime_error
{
	StateMachineGoneBad(const std::string& err)
	:runtime_error(err)
	{
	}
};


struct ServiceInfo
{
	string name, id;
	UUID uuid;
};

struct PrimaryService
{
	uint16_t start_handle;
	uint16_t end_handle;
	UUID uuid;
	vector<Characteristic> characteristics;
};


const ServiceInfo* lookup_service_by_UUID(const UUID& uuid)
{
	static vector<ServiceInfo> vec;

	if(vec.size() == 0)
	{
		auto add = [](const char* n, const char* i, uint16_t u)
		{
			ServiceInfo s;
			s.name=n;
			s.id = i;
			s.uuid = UUID(u);
			vec.push_back(s);
		};

		add( "Alert Notification Service",    "org.bluetooth.service.alert_notification",        0x1811);
		add( "Battery Service",               "org.bluetooth.service.battery_service",           0x180F);
		add( "Blood Pressure",                "org.bluetooth.service.blood_pressure",            0x1810);
		add( "Body Composition",              "org.bluetooth.service.body_composition",          0x181B);
		add( "Bond Management",               "org.bluetooth.service.bond_management",           0x181E);
		add( "Current Time Service",          "org.bluetooth.service.current_time",              0x1805);
		add( "Cycling Power",                 "org.bluetooth.service.cycling_power",             0x1818);
		add( "Cycling Speed and Cadence",     "org.bluetooth.service.cycling_speed_and_cadence", 0x1816);
		add( "Device Information",            "org.bluetooth.service.device_information",        0x180A);
		add( "Generic Access",                "org.bluetooth.service.generic_access",            0x1800);
		add( "Generic Attribute",             "org.bluetooth.service.generic_attribute",         0x1801);
		add( "Glucose",                       "org.bluetooth.service.glucose",                   0x1808);
		add( "Health Thermometer",            "org.bluetooth.service.health_thermometer",        0x1809);
		add( "Heart Rate",                    "org.bluetooth.service.heart_rate",                0x180D);
		add( "Human Interface Device",        "org.bluetooth.service.human_interface_device",    0x1812);
		add( "Immediate Alert",               "org.bluetooth.service.immediate_alert",           0x1802);
		add( "Link Loss",                     "org.bluetooth.service.link_loss",                 0x1803);
		add( "Location and Navigation",       "org.bluetooth.service.location_and_navigation",   0x1819);
		add( "Next DST Change Service",       "org.bluetooth.service.next_dst_change",           0x1807);
		add( "Phone Alert Status Service",    "org.bluetooth.service.phone_alert_status",        0x180E);
		add( "Reference Time Update Service", "org.bluetooth.service.reference_time_update",     0x1806);
		add( "Running Speed and Cadence",     "org.bluetooth.service.running_speed_and_cadence", 0x1814);
		add( "Scan Parameters",               "org.bluetooth.service.scan_parameters",           0x1813);
		add( "Tx Power",                      "org.bluetooth.service.tx_power",                  0x1804);
		add( "User Data",                     "org.bluetooth.service.user_data",                 0x181C);
		add( "Weight Scale",                  "org.bluetooth.service.weight_scale",              0x181D);
	}

	auto f = find_if(vec.begin(), vec.end(), [&](const ServiceInfo& s)
	{
		return s.uuid == uuid;
	});

	if(f == vec.end())
		return 0;
	else
		return &*f;
}

class BLEGATTStateMachine
{
	private:

		static void buggerall(BLEGATTStateMachine&)
		{}

	public:
		BLEDevice dev;
		
		vector<PrimaryService> primary_services;

		States state = Idle;
		int next_handle_to_read=-1;
		int last_request=-1;
		
		vector<uint8_t> buf;


		struct PrimaryServiceInfo
		{
			string name, id;
			UUID uuid;
		};


	public:

		std::function<void(BLEGATTStateMachine&)> cb_connected = buggerall;
		std::function<void(BLEGATTStateMachine&)> cb_services_read = buggerall;
		std::function<void(BLEGATTStateMachine&)> cb_notify = buggerall;
		std::function<void(BLEGATTStateMachine&)> cb_find_characteristics = buggerall;
		std::function<void(BLEGATTStateMachine&)> cb_get_client_characteristic_configuration = buggerall;
		std::function<void(BLEGATTStateMachine&)> cb_write_response = buggerall;
		std::function<void(Characteristic&, const PDUNotificationOrIndication&)> cb_notify_or_indicate;


		BLEGATTStateMachine(const std::string& addr)
		:dev(addr)
		{
			buf.resize(128);
			cb_connected(*this);
		}

		int socket()
		{
			return dev.sock;
		}
		
		void reset()
		{
			state = Idle;
			next_handle_to_read=-1;
			last_request=-1;
		}

		void state_machine_write()
		{
			if(state == ReadingPrimaryService)
			{
				last_request = ATT_OP_READ_BY_GROUP_REQ;	
				dev.send_read_group_by_type(UUID(GATT_UUID_PRIMARY), next_handle_to_read, 0xffff);	
			}
			else if(state == FindAllCharacteristics)
			{
				last_request = ATT_OP_READ_BY_TYPE_REQ;	
				dev.send_read_by_type(UUID(GATT_CHARACTERISTIC), next_handle_to_read, 0xffff);	
			}
			else if(state == GetClientCharaceristicConfiguration)
			{
				last_request = ATT_OP_READ_BY_TYPE_REQ;	
				dev.send_read_by_type(UUID(GATT_CLIENT_CHARACTERISTIC_CONFIGURATION), next_handle_to_read, 0xffff);	
			}


		}

		void read_primary_services()
		{
			state = ReadingPrimaryService;
			next_handle_to_read=1;
			state_machine_write();
		}
	
		void find_all_characteristics()
		{
			if(state != Idle)
				throw "Error trying to issue command mid state\n";
			state = FindAllCharacteristics;
			next_handle_to_read=1;
			state_machine_write();
		}

		void get_client_characteristic_configuration()
		{
			if(state != Idle)
				throw "Error trying to issue command mid state\n";
			state = GetClientCharaceristicConfiguration;
			next_handle_to_read=1;
			state_machine_write();
		}

		void read_and_process_next()
		{
			LOG(Debug, "State is: " << state);
			PDUResponse r = dev.receive(buf);

			if(r.type() == ATT_OP_HANDLE_NOTIFY || r.type() == ATT_OP_HANDLE_IND)
			{
				PDUNotificationOrIndication n(r);
				//Find the correct characteristic
				for(auto& s:primary_services)
					if(n.handle() > s.start_handle && n.handle() <= s.end_handle)
						for(auto& c:s.characteristics)
							if(n.handle() == c.value_handle)
							{
								if(c.cb_notify_or_indicate)
									c.cb_notify_or_indicate(n);
								else
									cb_notify_or_indicate(c, n);
							}
				
				//Respond to indications after the callback has run
				if(!n.notification())
					dev.send_handle_value_confirmation();
			}
			else if(r.type() == ATT_OP_ERROR && PDUErrorResponse(r).request_opcode() != last_request)
			{
				PDUErrorResponse err(r);
				
				std::string msg = string("Unexpected opcode in error. Expected ") + att_op2str(last_request) + " got "  + att_op2str(err.request_opcode());
				LOG(Error, msg);
				reset(); // And hope for the best
				throw StateMachineGoneBad(msg);
			}
			else if(r.type() != ATT_OP_ERROR && r.type() != last_request + 1)
			{
				string msg = string("Unexpected response. Expected ") + att_op2str(last_request+1) + " got "  + att_op2str(r.type());
				LOG(Error, msg);
				reset(); // And hope for the best
				throw StateMachineGoneBad(msg);
			}
			else
			{
				if(state == ReadingPrimaryService)
				{
					if(r.type() == ATT_OP_ERROR)
					{
						if(PDUErrorResponse(r).error_code() == ATT_ECODE_ATTR_NOT_FOUND)
						{
							//Maybe ? Indicates that the last one has been read.
							cb_services_read(*this);
							reset();
						}
						else
						{
							PDUErrorResponse err(r);
							string msg = string("Received unexpected error:") + att_ecode2str(err.error_code());
							LOG(Error, msg);
							reset(); // And hope for the best
							throw StateMachineGoneBad(msg);
						}
					}
					else
					{
						GATTReadServiceGroup g(r);
						
						for(int i=0; i < g.num_elements(); i++)
						{
							struct PrimaryService service;
							service.start_handle = g.start_handle(i);
							service.end_handle   = g.end_handle(i);
							service.uuid         = UUID::from(g.uuid(i));
							primary_services.push_back(service);
						}
						

						if(primary_services.back().end_handle == 0xffff)
						{
							reset();
							cb_services_read(*this);
						}
						else
						{
							next_handle_to_read = primary_services.back().end_handle+1;
							state_machine_write();
						}
					}
				}
				else if(state == FindAllCharacteristics)
				{
					if(r.type() == ATT_OP_ERROR)
					{
						if(PDUErrorResponse(r).error_code() == ATT_ECODE_ATTR_NOT_FOUND)
						{
							//Maybe ? Indicates that the last one has been read.
							reset();
							cb_find_characteristics(*this);
						}
						else
						{
							PDUErrorResponse err(r);
							string msg = string("Received unexpected error:") + att_ecode2str(err.error_code());
							LOG(Error, msg);
							reset(); // And hope for the best
							throw StateMachineGoneBad(msg);
						}
					}
					else
					{
						GATTReadCharacteristic rc(r);

						for(int i=0; i < rc.num_elements(); i++)
						{
							uint16_t handle = rc.handle(i);
							GATTReadCharacteristic::Characteristic ch = rc.characteristic(i);

							LOG(Debug, "Found characteristic handle: " << to_hex(handle));

							//Search for the correct service.
							for(unsigned int s=0; s < primary_services.size(); s++)
							{
								if(handle > primary_services[s].start_handle && handle <= primary_services[s].end_handle)
								{
									LOG(Debug, "  handle belongs to service " << s);
									Characteristic c(this);


									c.broadcast= ch.flags & GATT_CHARACTERISTIC_FLAGS_BROADCAST;
									c.read     = ch.flags & GATT_CHARACTERISTIC_FLAGS_READ;
									c.write_without_response= ch.flags & GATT_CHARACTERISTIC_FLAGS_WRITE_WITHOUT_RESPONSE;
									c.write    = ch.flags & GATT_CHARACTERISTIC_FLAGS_WRITE;
									c.notify   = ch.flags & GATT_CHARACTERISTIC_FLAGS_NOTIFY;
									c.indicate = ch.flags & GATT_CHARACTERISTIC_FLAGS_INDICATE;
									c.authenticated_write = ch.flags & GATT_CHARACTERISTIC_FLAGS_AUTHENTICATED_SIGNED_WRITES;
									c.extended = ch.flags & GATT_CHARACTERISTIC_FLAGS_EXTENDED_PROPERTIES;
									c.uuid     = UUID::from(ch.uuid);
									c.value_handle = ch.handle;
									c.client_characteric_configuration_handle = 0;
									c.first_handle = handle;
									
									//Initially mark the end as the start of the current service
									c.last_handle = primary_services[s].end_handle;

									//Terminate the previous characteristic
									if(!primary_services[s].characteristics.empty())
										primary_services[s].characteristics.back().last_handle = handle-1;
									
									primary_services[s].characteristics.push_back(c);



								}
							}

							next_handle_to_read = handle+1;
						}
						LOG(Debug,  "Reading " << to_hex((uint16_t)next_handle_to_read) << " next");
						state_machine_write();
					}
				}
				else if(state == GetClientCharaceristicConfiguration)
				{
					if(r.type() == ATT_OP_ERROR)
					{
						if(PDUErrorResponse(r).error_code() == ATT_ECODE_ATTR_NOT_FOUND)
						{
							//Maybe ? Indicates that the last one has been read.
							reset();
							cb_get_client_characteristic_configuration(*this);
						}
						else
						{
							PDUErrorResponse err(r);
							string msg = string("Received unexpected error:") + att_ecode2str(err.error_code());
							LOG(Error, msg);
							reset(); // And hope for the best
							throw StateMachineGoneBad(msg);
						}
					}
					else
					{
						GATTReadCCC rc(r);

						for(int i=0; i < rc.num_elements(); i++)
						{
							uint16_t handle = rc.handle(i);
							next_handle_to_read = handle + 1;
							LOG(Debug, "Handle: " << to_hex(rc.handle(i)) << "  ccc: " << to_hex(rc.ccc(i)));


							//Find the correct place
							for(auto& s:primary_services)
								if(handle > s.start_handle && handle <= s.end_handle)
									for(auto& c:s.characteristics)
										if(handle > c.first_handle && handle <= c.last_handle)
										{
											c.client_characteric_configuration_handle = rc.handle(i);
											c.ccc_last_known_value = rc.ccc(i);
										}
										
						}
						state_machine_write();
					}
				}
				else if(state == AwaitingWriteResponse)
				{

					if(r.type() == ATT_OP_ERROR)
					{
						PDUErrorResponse err(r);
						string msg = string("Received unexpected error:") + att_ecode2str(err.error_code());
						LOG(Error, msg);
						throw StateMachineGoneBad(msg);
					}
					else
					{
						reset();
						cb_write_response(*this);
					}
				}
			}
		}

		

		void set_notify_and_indicate(Characteristic& c, bool notify, bool indicate)
		{
			LOG(Trace, "BLEGATTStateMachine::enable_indications(Characteristic&)");

			if(state != Idle)
				throw "Error trying to issue command mid state\n";
			
			if(!c.indicate && indicate)
				throw("Error: this is not indicateable");
			if(!c.notify && notify)
				throw("Error: this is not notifiable");

			//FIXME: check for CCC
			c.ccc_last_known_value = notify | (indicate << 1);
			dev.send_write_command(c.client_characteric_configuration_handle, c.ccc_last_known_value);
		}
};


void Characteristic::set_notify_and_indicate(bool notify, bool indicate)
{
	LOG(Trace, "Characteristic::enable_indications()");
	s->set_notify_and_indicate(*this, notify, indicate);
}


void pretty_print_tree(const BLEGATTStateMachine& s)
{

	cout << "Primary services:\n";
	for(auto& service: s.primary_services)
	{
		cout << "Start: " << to_hex(service.start_handle);
		cout << " End:  " << to_hex(service.end_handle);
		cout << " UUID: " << to_str(service.uuid) << endl;
		const ServiceInfo* s = lookup_service_by_UUID(UUID::from(service.uuid));
		if(s)
			cout << "  " << s->id << ": " << s->name << endl;
		else
			cout << "  Unknown\n";


		for(auto& characteristic: service.characteristics)
		{
			cout  << "  Characteristic: " << to_str(characteristic.uuid) << endl;
			cout  << "   Start: " << to_hex(characteristic.first_handle) << "  End: " << to_hex(characteristic.last_handle) << endl;

			cout << "   Flags: ";
			if(characteristic.broadcast)
				cout << "Broadcast ";
			if(characteristic.read)
				cout << "Read ";
			if(characteristic.write_without_response)
				cout << "Write (without response) ";
			if(characteristic.write)
				cout << "Write ";
			if(characteristic.notify)
				cout << "Notify ";
			if(characteristic.indicate)
				cout << "Indicate ";
			if(characteristic.authenticated_write)
				cout << "Authenticated signed writes ";
			if(characteristic.extended)
				cout << "Extended properties ";
			cout  << endl;

			cout << "   Value at handle: " << characteristic.value_handle << endl;

			if(characteristic.client_characteric_configuration_handle != 0)
				cout << "   CCC: (" << to_hex(characteristic.client_characteric_configuration_handle) << ") " << to_hex(characteristic.ccc_last_known_value) << endl;

			cout << endl;

		}

		cout << endl;
	}
}


int main(int argc, char **argv)
{
	if(argc != 2)
	{	
		cerr << "Please supply address.\n";
		exit(1);
	}

	log_level = Warning;
	vector<uint8_t> buf(256);

#if 1
	BLEGATTStateMachine gatt(argv[1]);

	
	gatt.cb_services_read = [](BLEGATTStateMachine& s)
	{
		s.find_all_characteristics();
	};


	gatt.cb_find_characteristics = [](BLEGATTStateMachine& s)
	{
		s.get_client_characteristic_configuration();
	};

	gatt.cb_get_client_characteristic_configuration = [](BLEGATTStateMachine& s)
	{
		for(auto& service: s.primary_services)
			for(auto& characteristic: service.characteristics)
				if(service.uuid == UUID(0x1809) && characteristic.uuid == UUID(0x2a1c))
				{
					characteristic.cb_notify_or_indicate = [](const PDUNotificationOrIndication& n)
					{
						cerr << "Hello: " << (float)n.value().first[1] / 10. << endl;

					};

					characteristic.set_notify_and_indicate(false, true);
				}
	};


	gatt.read_primary_services();
	
	try{
		for(;;)
		{
			gatt.read_and_process_next();
		}
	}
	catch(const char* err)
	{
		cerr << "Fuck: " << err << endl;
	}

	


#endif

#if 0
	SimpleBlockingGATTDevice b(argv[1]);
	
	bt_uuid_t uuid;
	uuid.type = BT_UUID16;

	uuid.value.u16 = 0x2800;
	
	
	log_level=Warning;

	/*auto r = b.read_by_type(uuid);

	for(unsigned int i=0; i < r.size(); i++)
	{
		cout << "Handle: " << to_hex(r[i].first) << ", Data: " << to_hex(r[i].second) << endl;
		cout <<                "-->" << to_str(r[i].second) << "<--" << endl;
	}*/


	cout << "Primary:\n";
	auto s = b.read_service_group(uuid);
		
	for(unsigned int i=0; i < s.size(); i++)
	{
		cout << "Start: " << to_hex(get<0>(s[i]));
		cout << " End: " << to_hex(get<1>(s[i]));
		cout << " UUID: " << to_str(get<2>(s[i])) << endl;
	}
	
	//Note that characteristics form a group.
	cout << "Characteristic\n";
	auto r1 = b.read_characaristic();
	for(const auto& i: r1)
	{
		cout << to_hex(i.first) << " Value handle: " << to_hex(i.second.handle) << "  UUID: " << to_str(i.second.uuid)<<  " Flags: ";
		if(i.second.flags & GATT_CHARACTERISTIC_FLAGS_BROADCAST)
			cout << "Broadcast ";
		if(i.second.flags & GATT_CHARACTERISTIC_FLAGS_READ)
			cout << "Read ";
		if(i.second.flags & GATT_CHARACTERISTIC_FLAGS_WRITE_WITHOUT_RESPONSE)
			cout << "Write (without response) ";
		if(i.second.flags & GATT_CHARACTERISTIC_FLAGS_WRITE)
			cout << "Write ";
		if(i.second.flags & GATT_CHARACTERISTIC_FLAGS_NOTIFY)
			cout << "Notify ";
		if(i.second.flags & GATT_CHARACTERISTIC_FLAGS_INDICATE)
			cout << "Indicate ";
		if(i.second.flags & GATT_CHARACTERISTIC_FLAGS_AUTHENTICATED_SIGNED_WRITES)
			cout << "Authenticated signed writes ";
		if(i.second.flags & GATT_CHARACTERISTIC_FLAGS_EXTENDED_PROPERTIES)
			cout << "Extended properties ";
		cout << endl;
	}

	cout << "Information\n";
	auto r2 = b.find_information();
	for(const auto &i: r2)
	{
		cout << "Handle: " << to_hex(i.first) << "	Type: " << to_str(i.second) << endl;
	}

	/*b.send_read_by_type(uuid);
	b.receive(buf);

	uuid.value.u16 = 0x2901;
	b.send_read_by_type(uuid);
	b.receive(buf);

	uuid.value.u16 = 0x2803;
	b.send_read_by_type(uuid);
	b.receive(buf);

	b.send_read_by_type(uuid, 0x0008);
	b.receive(buf);
	b.send_read_by_type(uuid, 0x0010);
	b.receive(buf);
	b.send_read_by_type(uuid, 0x0021);
	b.receive(buf);


	cerr << endl<<endl<<endl;
*/

/*
	cerr << endl<<endl<<endl;
	cerr << endl<<endl<<endl;
	cerr << endl<<endl<<endl;
	uuid.value.u16 = 0x2800;
	b.send_read_by_type(uuid);
	b.receive(buf);
	b.send_read_group_by_type(uuid);
	b.receive(buf);
	b.send_read_group_by_type(uuid, 0x0013);
	b.receive(buf);
*/

	#endif

}

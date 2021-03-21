#include <sstream>

#include "Poco/Tuple.h"
#include "Poco/Runnable.h"
#include "Poco/Net/HTTPClientSession.h"
#include "Poco/Net/HTTPRequest.h"
#include "Poco/Net/HTTPResponse.h"
#include "Poco/StreamCopier.h"
#include "Poco/Path.h"
#include "Poco/URI.h"
#include "Poco/Exception.h"
#include "Poco/DOM/DOMParser.h"
#include "Poco/DOM/Document.h"
#include "Poco/DOM/NodeIterator.h"
#include "Poco/DOM/NodeFilter.h"
#include "Poco/DOM/AutoPtr.h"
#include "Poco/SAX/InputSource.h"
#include "Poco/DigestStream.h"
#include "Poco/MD5Engine.h"
#include "Poco/UTF16Encoding.h"
#include "Poco/UnicodeConverter.h"
#include "Poco/Notification.h"
#include "Poco/NotificationQueue.h"
#include "Poco/AutoPtr.h"
#include "Poco/NumberParser.h"

#include "AbstractOpenHAT.h"

// requires PAHO C libraries, see https://github.com/eclipse/paho.mqtt.cpp
#include "mqtt/client.h"

namespace {

class MQTTPlugin;

class MQTTPort  {
friend class MQTTPlugin;
protected:
	std::string friendlyname;
	bool valueSet;
public:
	MQTTPort(const std::string& friendlyname) {
		this->friendlyname = friendlyname;
		this->valueSet = false;
	};
};

class ActionNotification : public Poco::Notification {
public:
	typedef Poco::AutoPtr<ActionNotification> Ptr;

	enum ActionType {
		LOGIN,
		LOGOUT,
		GETSWITCHSTATE,
		SETSWITCHSTATEHIGH,
		SETSWITCHSTATELOW,
		GETPOWER,
		GETENERGY,
		GETTEMPERATURE
	};

	ActionType type;
	MQTTPort* port;

	ActionNotification(ActionType type, MQTTPort* port);
};

ActionNotification::ActionNotification(ActionType type, MQTTPort* port) {
	this->type = type;
	this->port = port;
}

////////////////////////////////////////////////////////////////////////
// Plugin main class
////////////////////////////////////////////////////////////////////////

class MQTTPlugin : public IOpenHATPlugin, public openhat::IConnectionListener, public Poco::Runnable {
	friend class HG06337Switch;
protected:
	class user_callback : public virtual mqtt::callback
	{
		void connection_lost(const std::string& cause) override {
			std::cout << "\nConnection lost" << std::endl;
			if (!cause.empty())
				std::cout << "\tcause: " << cause << std::endl;
		}

		void delivery_complete(mqtt::delivery_token_ptr tok) override {
			std::cout << "\n\t[Delivery complete for token: "
				<< (tok ? tok->get_message_id() : -1) << "]" << std::endl;
		}

	public:
	};

	std::string nodeID;

	std::string host;
	int port;
	std::string user;
	std::string password;
	int timeoutSeconds;
	int QOS;

	std::string sid;
	int errorCount;
	std::string lastErrorMessage;

	opdi::LogVerbosity logVerbosity;

	Poco::NotificationQueue queue;

	Poco::Thread workThread;
	
	mqtt::client *client;
	
	void publish(const std::string& topic, const std::string& payload);

	void errorOccurred(const std::string& message);

public:
	openhat::AbstractOpenHAT* openhat;

	virtual void masterConnected(void) override;
	virtual void masterDisconnected(void) override;

	virtual void run(void);

	virtual void setupPlugin(openhat::AbstractOpenHAT* abstractOpenHAT, const std::string& node, openhat::ConfigurationView::Ptr nodeConfig, openhat::ConfigurationView::Ptr parentConfig, const std::string& driverPath) override;
};

////////////////////////////////////////////////////////////////////////
// Plugin ports
////////////////////////////////////////////////////////////////////////
class HG06337Switch : public opdi::DigitalPort, public MQTTPort {
	friend class MQTTPlugin;
protected:
	MQTTPlugin* plugin;
	Poco::Mutex mutex;
	int8_t switchState;

	virtual void setSwitchState(int8_t line);

public:
	HG06337Switch(MQTTPlugin* plugin, const std::string& id, const std::string& friendlyname);

	virtual void setLine(uint8_t line, ChangeSource changeSource = opdi::Port::ChangeSource::CHANGESOURCE_INT) override;

	virtual uint8_t doWork(uint8_t canSend) override;
};

}	// end anonymous namespace
/*
class FritzDECT200Power : public opdi::DialPort, public FritzPort {
	friend class FritzBoxPlugin;
protected:
	FritzBoxPlugin* plugin;

	int32_t power;

	virtual void setPower(int32_t power);

public:
	FritzDECT200Power(FritzBoxPlugin* plugin, const std::string& id, const std::string& ain, int queryInterval);

	virtual uint8_t doWork(uint8_t canSend) override;
};

class FritzDECT200Energy : public opdi::DialPort, public FritzPort {
	friend class FritzBoxPlugin;
protected:
	FritzBoxPlugin* plugin;

	int32_t energy;

	virtual void setEnergy(int32_t energy);

public:
	FritzDECT200Energy(FritzBoxPlugin* plugin, const std::string& id, const std::string& ain, int queryInterval);

	virtual uint8_t doWork(uint8_t canSend) override;
};

class FritzDECT200Temperature : public opdi::DialPort, public FritzPort {
	friend class FritzBoxPlugin;
protected:
	FritzBoxPlugin* plugin;

	int32_t temperature;

	virtual void setTemperature(int32_t temperature);

public:
	FritzDECT200Temperature(FritzBoxPlugin* plugin, const std::string& id, const std::string& ain, int queryInterval);

	virtual uint8_t doWork(uint8_t canSend) override;
};


*/
HG06337Switch::HG06337Switch(MQTTPlugin* plugin, const std::string& id, const std::string& friendlyname) : 
	opdi::DigitalPort(id.c_str(), OPDI_PORTDIRCAP_OUTPUT, 0), MQTTPort(friendlyname) {
	this->plugin = plugin;
	this->switchState = -1;	// unknown
	this->valueSet = true;		// causes setError in doWork
	
	this->mode = OPDI_DIGITAL_MODE_OUTPUT;
	this->setIcon("powersocket");
}

uint8_t HG06337Switch::doWork(uint8_t canSend) {
	opdi::DigitalPort::doWork(canSend);

	/*
	// time for refresh?
	if (opdi_get_time_ms() - this->lastQueryTime > this->queryInterval * 1000) {
		this->plugin->queue.enqueueNotification(new ActionNotification(ActionNotification::GETSWITCHSTATE, this));
		this->lastQueryTime = opdi_get_time_ms();
	}
*/
	Poco::Mutex::ScopedLock lock(this->mutex);

	// has a value been returned?
	if (this->valueSet) {
		// values < 0 signify an error
		if (this->switchState > -1)
			// use base method to avoid triggering an action!
			opdi::DigitalPort::setLine(this->switchState);
		else
			this->setError(Error::VALUE_NOT_AVAILABLE);
		this->valueSet = false;
	}

	return OPDI_STATUS_OK;
}

void HG06337Switch::setLine(uint8_t line, ChangeSource changeSource) {
	opdi::DigitalPort::setLine(line, changeSource);

	if (line == 0)
		this->plugin->queue.enqueueNotification(new ActionNotification(ActionNotification::SETSWITCHSTATELOW, this));
	else
	if (line == 1)
		this->plugin->queue.enqueueNotification(new ActionNotification(ActionNotification::SETSWITCHSTATEHIGH, this));
}

void HG06337Switch::setSwitchState(int8_t switchState) {
	Poco::Mutex::ScopedLock lock(this->mutex);

	this->switchState = switchState;
	this->valueSet = true;
}

/*
FritzDECT200Power::FritzDECT200Power(FritzBoxPlugin* plugin, const std::string& id, const std::string& ain, int queryInterval) : 
	opdi::DialPort(id.c_str()), FritzPort(id, ain, queryInterval) {
	this->plugin = plugin;
	this->power = -1;	// unknown
	this->lastQueryTime = 0;
	this->valueSet = true;		// causes setError in doWork

	this->minValue = 0;
	this->maxValue = 2300000;	// measured in mW; 2300 W is maximum power load for the DECT200
	this->step = 1;
	this->position = 0;

	this->setUnit("electricPower_mW");
	this->setIcon("powermeter");

	// port is readonly
	this->setReadonly(true);
}

uint8_t FritzDECT200Power::doWork(uint8_t canSend) {
	opdi::DialPort::doWork(canSend);

	// time for refresh?
	if (opdi_get_time_ms() - this->lastQueryTime > this->queryInterval * 1000) {
		this->plugin->queue.enqueueNotification(new ActionNotification(ActionNotification::GETPOWER, this));
		this->lastQueryTime = opdi_get_time_ms();
	}

	Poco::Mutex::ScopedLock lock(this->mutex);

	// has a value been returned?
	if (this->valueSet) {
		// values < 0 signify an error
		if (this->power > -1)
			opdi::DialPort::setPosition(this->power);
		else
			this->setError(Error::VALUE_NOT_AVAILABLE);
		this->valueSet = false;
	}

	return OPDI_STATUS_OK;
}

void FritzDECT200Power::setPower(int32_t power) {
	Poco::Mutex::ScopedLock lock(this->mutex);

	this->power = power;
	this->valueSet = true;
}

FritzDECT200Energy::FritzDECT200Energy(FritzBoxPlugin* plugin, const std::string& id, const std::string& ain, int queryInterval) : 
	opdi::DialPort(id.c_str()), FritzPort(id, ain, queryInterval) {
	this->plugin = plugin;
	this->energy = -1;	// unknown
	this->lastQueryTime = 0;
	this->valueSet = true;		// causes setError in doWork

	this->minValue = 0;
	this->maxValue = 2147483647;	// measured in Wh
	this->step = 1;
	this->position = 0;

	this->setUnit("electricEnergy_Wh");
	this->setIcon("energymeter");

	// port is readonly
	this->setReadonly(true);
}

uint8_t FritzDECT200Energy::doWork(uint8_t canSend) {
	opdi::DialPort::doWork(canSend);

	// time for refresh?
	if (opdi_get_time_ms() - this->lastQueryTime > this->queryInterval * 1000) {
		this->plugin->queue.enqueueNotification(new ActionNotification(ActionNotification::GETENERGY, this));
		this->lastQueryTime = opdi_get_time_ms();
	}

	Poco::Mutex::ScopedLock lock(this->mutex);

	// has a value been returned?
	if (this->valueSet) {
		// values < 0 signify an error
		if (this->energy > -1)
			opdi::DialPort::setPosition(this->energy);
		else
			this->setError(Error::VALUE_NOT_AVAILABLE);
		this->valueSet = false;
	}

	return OPDI_STATUS_OK;
}

void FritzDECT200Energy::setEnergy(int32_t energy) {
	Poco::Mutex::ScopedLock lock(this->mutex);

	this->energy = energy;
	this->valueSet = true;
}

FritzDECT200Temperature::FritzDECT200Temperature(FritzBoxPlugin* plugin, const std::string& id, const std::string& ain, int queryInterval) :
	opdi::DialPort(id.c_str()), FritzPort(id, ain, queryInterval) {
	this->plugin = plugin;
	this->temperature = -9999;	// unknown
	this->lastQueryTime = 0;
	this->valueSet = true;		// causes setError in doWork

	// value is measured in centidegrees Celsius
	this->minValue = -1000;			// -100°C
	this->maxValue = 1000;			// +100°C
	this->step = 1;
	this->position = 0;

	this->setUnit("temperature_centiDegreesCelsius");
	this->setIcon("thermometer_celsius");

	// port is readonly
	this->setReadonly(true);
}

uint8_t FritzDECT200Temperature::doWork(uint8_t canSend) {
	opdi::DialPort::doWork(canSend);

	// time for refresh?
	if (opdi_get_time_ms() - this->lastQueryTime > this->queryInterval * 1000) {
		this->plugin->queue.enqueueNotification(new ActionNotification(ActionNotification::GETTEMPERATURE, this));
		this->lastQueryTime = opdi_get_time_ms();
	}

	Poco::Mutex::ScopedLock lock(this->mutex);

	// has a value been returned?
	if (this->valueSet) {
		// values <= -9999 signifies an error
		if (this->temperature > -9999)
			opdi::DialPort::setPosition(this->temperature);
		else
			this->setError(Error::VALUE_NOT_AVAILABLE);
		this->valueSet = false;
	}

	return OPDI_STATUS_OK;
}

void FritzDECT200Temperature::setTemperature(int32_t temperature) {
	Poco::Mutex::ScopedLock lock(this->mutex);

	this->temperature = temperature;
	this->valueSet = true;
}
*/
////////////////////////////////////////////////////////
// Plugin implementation
////////////////////////////////////////////////////////

// Credits: Sir Slick, https://stackoverflow.com/questions/2589096/find-most-significant-bit-left-most-that-is-set-in-a-bit-array
unsigned int msb32(unsigned int x)
{
	static const unsigned int bval[] =
	{ 0,1,2,2,3,3,3,3,4,4,4,4,4,4,4,4 };

	unsigned int r = 0;
	if (x & 0xFFFF0000) { r += 16 / 1; x >>= 16 / 1; }
	if (x & 0x0000FF00) { r += 16 / 2; x >>= 16 / 2; }
	if (x & 0x000000F0) { r += 16 / 4; x >>= 16 / 4; }
	return r + bval[x];
}

void MQTTPlugin::publish(const std::string& topic, const std::string& payload) {
	this->openhat->logVerbose(this->nodeID + ": Sending message to '" + topic + "': '" + payload + "'", this->logVerbosity);

	this->client->publish(mqtt::message(topic, payload, QOS, false));
}

void MQTTPlugin::errorOccurred(const std::string& message) {
	// This method is for errors that are usually logged in verbosity Normal.
	// It suppresses frequent occurrences of the same error.
	// identical error message?
	if (this->lastErrorMessage == message) {
		this->errorCount++;
	} else {
		// different error, reset counter
		this->errorCount = 1;
		this->lastErrorMessage = message;
	}
	// the error message is output if only the highest bit is set causing exponentially decreasing output frequency
	// get highest bit
	unsigned int msb = msb32(this->errorCount);
	// are the lower bits zero?
	if ((this->errorCount & ((1 << (msb - 1)) - 1)) == 0) {
		// add occurrence count if greater than 1
		this->openhat->logNormal((this->errorCount > 1 ? "(" + this->openhat->to_string(this->errorCount) + ") " : "") + message, this->logVerbosity);
	}
}
void MQTTPlugin::setupPlugin(openhat::AbstractOpenHAT* abstractOpenHAT, const std::string& node, openhat::ConfigurationView::Ptr nodeConfig, openhat::ConfigurationView::Ptr parentConfig, const std::string& /*driverPath*/) {
	this->openhat = abstractOpenHAT;
	this->nodeID = node;
	this->timeoutSeconds = 5;			// short timeout (assume local network)

	this->errorCount = 0;

	// get host and credentials
	this->host = this->openhat->getConfigString(nodeConfig, node, "Host", "", true);
	this->port = nodeConfig->getInt("Port", 1883);
	this->user = this->openhat->getConfigString(nodeConfig, node, "User", "", false);
	this->password = this->openhat->getConfigString(nodeConfig, node, "Password", "", false);
	this->timeoutSeconds = nodeConfig->getInt("Timeout", this->timeoutSeconds);
	this->QOS = 1;
	
	this->client = nullptr;

	// store main node's group (will become the default of ports)
	std::string group = nodeConfig->getString("Group", "");

	// store main node's verbosity level (will become the default of ports)
	this->logVerbosity = this->openhat->getConfigLogVerbosity(nodeConfig, this->openhat->getLogVerbosity());

	// enumerate keys of the plugin's nodes (in specified order)
	this->openhat->logVerbose(node + ": Enumerating MQTT devices: " + node + ".Devices", this->logVerbosity);

	Poco::AutoPtr<openhat::ConfigurationView> nodes = this->openhat->createConfigView(nodeConfig, "Devices");
	nodeConfig->addUsedKey("Devices");

	// get ordered list of devices
	openhat::ConfigurationView::Keys deviceKeys;
	nodes->keys("", deviceKeys);

	typedef Poco::Tuple<int, std::string> Item;
	typedef std::vector<Item> ItemList;
	ItemList orderedItems;

	// create ordered list of port keys (by priority)
	for (auto it = deviceKeys.begin(), ite = deviceKeys.end(); it != ite; ++it) {

		int itemNumber = nodes->getInt(*it, 0);
		// check whether the item is active
		if (itemNumber < 0)
			continue;

		// insert at the correct position to create a sorted list of items
		auto nli = orderedItems.begin();
		auto nlie = orderedItems.end();
		while (nli != nlie) {
			if (nli->get<0>() > itemNumber)
				break;
			++nli;
		}
		Item item(itemNumber, *it);
		orderedItems.insert(nli, item);
	}

	if (orderedItems.size() == 0) {
		this->openhat->logWarning("No devices configured in " + node + ".Devices; is this intended?");
	}

	// go through items, create ports in specified order
	auto nli = orderedItems.begin();
	auto nlie = orderedItems.end();
	while (nli != nlie) {
		std::string deviceName = nli->get<1>();

		this->openhat->logVerbose(node + ": Setting up MQTT device: " + deviceName, this->logVerbosity);

		// get device section from the configuration>
		Poco::AutoPtr<openhat::ConfigurationView> deviceConfig = this->openhat->createConfigView(parentConfig, deviceName);

		// get device type (required)
		std::string deviceType = this->openhat->getConfigString(deviceConfig, deviceName, "Type", "", true);

/*
		int queryInterval = deviceConfig->getInt("QueryInterval", 30);
		if (queryInterval < 1)
			this->openhat->throwSettingException(deviceName + ": Please specify a QueryInterval greater than 1: " + this->openhat->to_string(queryInterval));
 */
		if (deviceType == "HG06337") {
			// get friendly name (required)
			std::string friendlyname = this->openhat->getConfigString(deviceConfig, deviceName, "FriendlyName", "", true);

			this->openhat->logVerbose(node + ": Setting up HG06337 device port: " + deviceName + "_Switch", this->logVerbosity);

			if (parentConfig->hasProperty(deviceName + "_Switch.Type")) {
				this->openhat->logVerbose(node + ": Setting up HG06337 device port: " + deviceName + "_Switch", this->logVerbosity);
				Poco::AutoPtr<openhat::ConfigurationView> portConfig = this->openhat->createConfigView(parentConfig, deviceName + "_Switch");
				// check type
				if (portConfig->getString("Type") != "DigitalPort")
					this->openhat->throwSettingException(node + ": HG06337 Switch device port must be of type 'DigitalPort'");
				// setup the switch port instance and add it
				HG06337Switch* switchPort = new HG06337Switch(this, deviceName + "_Switch", friendlyname);
				// set default group: MQTT's node's group
				switchPort->setGroup(group);
				// set default log verbosity
				switchPort->logVerbosity = this->logVerbosity;
				// allow only basic settings to be changed
				this->openhat->configurePort(portConfig, switchPort, 0);
				this->openhat->addPort(switchPort);
			} else {
				this->openhat->logVerbose(node + ": HG06337 device port: " + deviceName + "_Switch.Type not found, ignoring", this->logVerbosity);
			}
		}
/*
			if (parentConfig->hasProperty(deviceName + "_Power.Type")) {
				this->openhat->logVerbose(node + ": Setting up FritzBoxPlugin device port: " + deviceName + "_Power", this->logVerbosity);
				Poco::AutoPtr<openhat::ConfigurationView> portConfig = this->openhat->createConfigView(parentConfig, deviceName + "_Power");
				// check type
				if (portConfig->getString("Type") != "DialPort")
					this->openhat->throwSettingException(node + ": FritzDECT200 Power device port must be of type 'DialPort'");
				// setup the power port instance and add it
				FritzDECT200Power* powerPort = new FritzDECT200Power(this, deviceName + "_Power", ain, queryInterval);
				// set default group: FritzBox's node's group
				powerPort->setGroup(group);
				// set default log verbosity
				powerPort->logVerbosity = this->logVerbosity;
				// allow only basic settings to be changed
				this->openhat->configurePort(portConfig, powerPort, 0);
				this->openhat->addPort(powerPort);
			} else {
				this->openhat->logVerbose(node + ": FritzBoxPlugin device port: " + deviceName + "_Power.Type not found, ignoring", this->logVerbosity);
			}

			if (parentConfig->hasProperty(deviceName + "_Energy.Type")) {
				this->openhat->logVerbose(node + ": Setting up FritzBoxPlugin device port: " + deviceName + "_Energy", this->logVerbosity);
				Poco::AutoPtr<openhat::ConfigurationView> portConfig = this->openhat->createConfigView(parentConfig, deviceName + "_Energy");
				// check type
				if (portConfig->getString("Type") != "DialPort")
					this->openhat->throwSettingException(node + ": FritzDECT200 Energy device port must be of type 'DialPort'");
				// setup the energy port instance and add it
				FritzDECT200Energy* energyPort = new FritzDECT200Energy(this, deviceName + "_Energy", ain, queryInterval);
				// set default group: FritzBox's node's group
				energyPort->setGroup(group);
				// set default log verbosity
				energyPort->logVerbosity = this->logVerbosity;
				// allow only basic settings to be changed
				this->openhat->configurePort(portConfig, energyPort, 0);
				this->openhat->addPort(energyPort);
			} else {
				this->openhat->logVerbose(node + ": FritzBoxPlugin device port: " + deviceName + "_Energy.Type not found, ignoring", this->logVerbosity);
			}

			if (parentConfig->hasProperty(deviceName + "_Temperature.Type")) {
				this->openhat->logVerbose(node + ": Setting up FritzBoxPlugin device port: " + deviceName + "_Temperature", this->logVerbosity);
				Poco::AutoPtr<openhat::ConfigurationView> portConfig = this->openhat->createConfigView(parentConfig, deviceName + "_Temperature");
				// check type
				if (portConfig->getString("Type") != "DialPort")
					this->openhat->throwSettingException(node + ": FritzDECT200 Temperature device port must be of type 'DialPort'");
				// setup the energy port instance and add it
				FritzDECT200Temperature* temperaturePort = new FritzDECT200Temperature(this, deviceName + "_Temperature", ain, queryInterval);
				// set default group: FritzBox's node's group
				temperaturePort->setGroup(group);
				// set default log verbosity
				temperaturePort->logVerbosity = this->logVerbosity;
				// allow only basic settings to be changed
				this->openhat->configurePort(portConfig, temperaturePort, 0);
				this->openhat->addPort(temperaturePort);
			} else {
				this->openhat->logVerbose(node + ": FritzBoxPlugin device port: " + deviceName + "_Temperature.Type not found, ignoring", this->logVerbosity);
			}

		} else
			this->openhat->throwSettingException(node + ": This plugin does not support the device type '" + deviceType + "'");
*/
		++nli;
	}

	// this->openhat->addConnectionListener(this);

	this->workThread.setName(node + " work thread");
	this->workThread.start(*this);

	this->openhat->logVerbose(node + ": MQTTPlugin setup completed successfully", this->logVerbosity);
}

void MQTTPlugin::masterConnected() {
}

void MQTTPlugin::masterDisconnected() {
}

void MQTTPlugin::run(void) {

	this->openhat->logVerbose(this->nodeID + ": MQTTPlugin worker thread started", this->logVerbosity);

//	sample_mem_persistence persist;
	
	while (!this->openhat->shutdownRequested) {
		
		if (this->client == nullptr) {
			this->openhat->logVerbose(this->nodeID + ": Initializing MQTT client", this->logVerbosity);
			// try to connect to the server
			std::string server_address = std::string("tcp://") + this->host + ":" + this->openhat->to_string(this->port);
			this->client = new mqtt::client(server_address, this->openhat->getSlaveName());
			
			user_callback cb;
			this->client->set_callback(cb);
			
			mqtt::connect_options connOpts;
			connOpts.set_keep_alive_interval(20);
			connOpts.set_clean_session(true);

			try {
				this->openhat->logVerbose(this->nodeID + ": Connecting to MQTT server: " + server_address, this->logVerbosity);
				mqtt::connect_response cr = this->client->connect(connOpts);
				this->openhat->logVerbose(this->nodeID + ": Connected to MQTT server: " + cr.get_server_uri(), this->logVerbosity);
			} catch (const mqtt::exception& exc) {
				this->openhat->logVerbose(this->nodeID + ": Error connecting to MQTT server " + server_address + ": " + exc.what(), this->logVerbosity);
				delete client;
				client = nullptr;
			} catch (const std::exception& exc) {
				this->openhat->logVerbose(this->nodeID + ": Error connecting to MQTT server " + server_address + ": " + exc.what(), this->logVerbosity);
				delete client;
				client = nullptr;
			}
			
			if (client == nullptr) {
				// sleep and try again
                struct timespec aSleep;
                struct timespec aRem;
				aSleep.tv_sec = 10;
				aSleep.tv_nsec = 0;
				nanosleep(&aSleep, &aRem);
			}
		}
/*
		try {
			Poco::Notification::Ptr notification = this->queue.waitDequeueNotification(100);
			if (!this->openhat->shutdownRequested && notification) {
				ActionNotification::Ptr workNf = notification.cast<ActionNotification>();
				if (workNf) {
					std::string action;
					switch (workNf->type) {
					case ActionNotification::LOGIN: action = "LOGIN"; break;
					case ActionNotification::LOGOUT: action = "LOGOUT"; break;
					case ActionNotification::GETSWITCHSTATE: action = "GETSWITCHSTATE"; break;
					case ActionNotification::SETSWITCHSTATELOW: action = "SETSWITCHSTATELOW"; break;
					case ActionNotification::SETSWITCHSTATEHIGH: action = "SETSWITCHSTATEHIGH"; break;
					case ActionNotification::GETENERGY: action = "GETENERGY"; break;
					case ActionNotification::GETPOWER: action = "GETPOWER"; break;
					case ActionNotification::GETTEMPERATURE: action = "GETTEMPERATURE"; break;
					}
					FritzPort* fp = (FritzPort*)workNf->port;
					std::string portID = fp->id;
					this->openhat->logDebug(this->nodeID + ": FritzBoxPlugin processing requested action: " + action + " for port: " + portID, this->logVerbosity);
					// inspect action and decide what to do
					switch (workNf->type) {
					case ActionNotification::LOGIN: this->login(); break;
					case ActionNotification::LOGOUT: this->logout(); break;
					case ActionNotification::GETSWITCHSTATE: this->getSwitchState(workNf->port); break;
					case ActionNotification::SETSWITCHSTATELOW: this->setSwitchState(workNf->port, 0); break;
					case ActionNotification::SETSWITCHSTATEHIGH: this->setSwitchState(workNf->port, 1); break;
					case ActionNotification::GETENERGY: this->getSwitchEnergy(workNf->port); break;
					case ActionNotification::GETPOWER: this->getSwitchPower(workNf->port); break;
					case ActionNotification::GETTEMPERATURE: this->getTemperature(workNf->port); break;
					}
				}
			}
		} catch (Poco::Exception &e) {
			this->openhat->logNormal(this->nodeID + ": Unhandled exception in worker thread: " + this->openhat->getExceptionMessage(e), this->logVerbosity);
		}
*/
	}

	this->openhat->logVerbose(this->nodeID + ": MQTTPlugin worker thread terminated", this->logVerbosity);
}

// plugin instance factory function

#ifdef _WINDOWS

extern "C" __declspec(dllexport) IOpenHATPlugin* __cdecl GetPluginInstance(int majorVersion, int minorVersion, int patchVersion)

#elif linux

extern "C" IOpenHATPlugin* GetPluginInstance(int majorVersion, int minorVersion, int /*patchVersion*/)

#else
#error "Unable to compile plugin instance factory function: Compiler not supported"
#endif
{
	// check whether the version is supported
	if ((majorVersion != openhat::OPENHAT_MAJOR_VERSION) || (minorVersion != openhat::OPENHAT_MINOR_VERSION))
		throw Poco::Exception("This plugin requires OpenHAT version "
			OPDI_QUOTE(openhat::OPENHAT_MAJOR_VERSION) "." OPDI_QUOTE(openhat::OPENHAT_MINOR_VERSION));

	// return a new instance of this plugin
	return new MQTTPlugin();
}

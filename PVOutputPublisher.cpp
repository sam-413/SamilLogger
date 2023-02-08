#include "PVOutputPublisher.h"

WiFiClient wificlient;
HTTPClient http; //our web client to perform post with

PVOutputPublisher::PVOutputPublisher(SettingsManager * settingsManager, SamilCommunicator * samiL, bool inDebugMode)
{
	pvOutputSettingsManager = settingsManager;
	samilCommunicator = samiL;
	debugMode = inDebugMode;
}


PVOutputPublisher::~PVOutputPublisher()
{
}

void PVOutputPublisher::start()
{
	if (!canStart())
	{
		Serial.println("PVOutput is disabled.");
		return;
	}
	if (debugMode)
		Serial.println("PVOutputPublisher started.");
	lastUpdated = millis();
	isStarted = true;
}

void PVOutputPublisher::stop()
{
	isStarted = false;
}

bool PVOutputPublisher::canStart()
{
	pvoutputSettings = pvOutputSettingsManager->GetSettings();
	return (pvoutputSettings->pvoutputApiKey.length() > 0);
}

bool PVOutputPublisher::getIsStarted()
{
	return isStarted;
}

void PVOutputPublisher::sendToPvOutput(SamilCommunicator::SamilInverterInformation info)
{
	//need to send out the data to pvouptut> use the avg values for pac, voltage and temp

	http.begin(wificlient, "http://pvoutput.org/service/r2/addstatus.jsp"); //Specify request destination
	http.addHeader("X-Pvoutput-Apikey", pvoutputSettings->pvoutputApiKey);
	http.addHeader("X-Pvoutput-SystemId", pvoutputSettings->pvoutputSystemId);
	http.addHeader("Content-Type", "application/x-www-form-urlencoded");

	//the inverter only reports eday with a .1 kWh resolution. This messus up the avg in pvoutput because the max resolution is 1200 Wh
	//we now the avg power in the last period so we can calc the new eday and compare it
	float eDay = info.eDay * 1000;
	if (avgCounter)
	{
		float avgWhPower = (float)(currentPacSum / avgCounter) / (60.0 * 60 * 1000 / (float)(millis() - lastUpdated));
		if (eDay - MAX_EDAY_DIFF < prevEday + avgWhPower && abs(prevEday + avgWhPower - eDay) < MAX_EDAY_DIFF) //when a new day starts the 'abs' part will reset to zero
			eDay = prevEday + avgWhPower;
	}
	prevEday = eDay;
	//construct our post message
	String postMsg = String("d=") + String(year()) + getZeroFilled(month()) + getZeroFilled(day());
	postMsg += String("&t=") + getZeroFilled(hour()) + ":" + getZeroFilled(minute());
	//v1 = total wh today
	postMsg += String("&v1=") + String(eDay, 0); //TODO: improve resolution by adding avg power to prev val

	//v2 = Power Generation
	if (avgCounter) //no datapoints recorded
	{
		if (debugMode)
		{
			Serial.print("Got some readings to calculate the avg power, temp and voltage. # readings: ");
			Serial.print(avgCounter);
			Serial.print(", pac sum: ");
			Serial.println(currentPacSum);
		}

		postMsg += String("&v2=") + String(currentPacSum / avgCounter); //improve resolution by adding avg power to prev val

		//v3 and v4 are power consumption (maybe doable using mqtt?)
		//v5 = temp
		postMsg += String("&v5=") + String(currentTempSum / avgCounter, 2);
		//v6 = voltage
		postMsg += String("&v6=") + String(currentVoltageSum / avgCounter, 2);
	}

	//v7 = custom 1 = vac1
	postMsg += String("&v7=") + String(info.vac1, 2);
	//v8 = custom 2 = iac1
	postMsg += String("&v8=") + String(info.iac1, 2);
	//v9 = custom 3 = fac1
	postMsg += String("&v9=") + String(info.fac1, 2);
	//v10 = custom 4 = vpv1
	postMsg += String("&v10=") + String(info.vpv1, 2);
	//v11 = custom 5 = vpv2
	postMsg += String("&v11=") + String(info.vpv2, 2);
	//v12 = custom 6 = errormsg
	postMsg += String("&v12=") + String(info.errorMessage);

	int httpCode = http.POST(postMsg); //Send the request
	String payload = http.getString();  //Get the response payload
	http.end();
	if (debugMode)
	{
		Serial.println(postMsg);
		Serial.print("Result: ");
		Serial.println(httpCode);
		Serial.print("Payload: ");
		Serial.println(payload);
	}
}

String PVOutputPublisher::getZeroFilled(int num)
{
	return  num < 10 ? "0" + String(num) : String(num);
}

void PVOutputPublisher::handle()
{
	if (!isStarted)
		return;
	//check if time elapsed and we need to send the current values
	//lastUpdated. For now we only support one inverter
	auto inverters = samilCommunicator->getInvertersInfo();
	if (inverters.size() > 0)
	{

		//check if we need to send the info to pvoutptut.
		//if not check if the pac value changed add it to the current sum so we can calc the average on sending
		if (wasOnline && millis() - lastUpdated > pvoutputSettings->pvoutputUpdateInterval)
		{
			//send it out
			sendToPvOutput(inverters[0]);
			ResetAverage();
			lastUpdated = millis();

			if (!inverters[0].isOnline && wasOnline)
			{
				//went offline. Data was sent for the last time
				wasOnline = false;
				//cleaar all avg data first
				ResetAverage();
			}
		}
		else
		{
			//keep track of when the inverter went offline
			if (!wasOnline && inverters[0].isOnline)
			{
				wasOnline = true;
				//cleaar all avg data first
				ResetAverage();
			}
			
			//check if inverter info was updated
			if (inverters[0].isOnline && (inverters[0].pac != lastPac || inverters[0].vpv1 + inverters[0].vpv2 != lastVoltage || inverters[0].temp != lastTemp))
			{
				//changed. so change the avg counters
				lastPac = inverters[0].pac;
				lastVoltage = inverters[0].vpv1 + inverters[0].vpv2;
				lastTemp = inverters[0].temp;
				currentPacSum += lastPac;
				currentVoltageSum += lastVoltage;
				currentTempSum += lastTemp;
				avgCounter += 1;
			}
		}
	}
}

void PVOutputPublisher::ResetAverage()
{
	//reset counter vals
	avgCounter = 0;
	currentPacSum = 0;
	currentVoltageSum = 0;
	currentTempSum = 0;
	lastPac = 0;
	lastVoltage = 0;
	lastTemp = 0;
}


if sht == nil then
	sht = i2c.attach(i2c.I2C1, i2c.MASTER, 100000)
end



function checkCRC(message_from_sensor, check_value_from_sensor)
	local remainder = message_from_sensor << 8
	remainder = remainder | check_value_from_sensor
	local divisor = 0x988000 --SHIFTED_DIVISOR
	for i = 1,16 do
		local calc = 1 << (24 - i)
		if remainder & calc ~= 0 then
			remainder = remainder ~ divisor
		end
		divisor = divisor >> 1
	end
	remainder = remainder & 0xFF -- (byte)remainder
	return remainder
end

function readValue(slave, command)
	sht:start()
	sht:address(slave, false)
	sht:write(command)
	sht:stop()

	tmr.delay(10) --DELAY_INTERVAL

	sht:start()
	sht:address(slave, true)
	local msb = sht:read()
	local lsb = sht:read()
	local crc = sht:read()
	sht:stop()

	local measure = (msb << 8 | lsb)
	if checkCRC(measure, crc) ~= 0 then
		return nil
	end

	return measure
end

function readTemperature()
	local measure = readValue(0x40, 0xF3) --TRIGGER_TEMP_MEASURE_NOHOLD
	if measure == nil then return measure end
	return (measure * 175.72 / 65536.0) - 46.85
end

function readHumidity()
	local measure = readValue(0x40, 0xF5) --TRIGGER_HUMD_MEASURE_NOHOLD
	if measure == nil then return measure end
	return (measure * 125.0 / 65536.0) - 6.0
end

-- original code see https://nachbelichtet.com/2019/09/19/richtig-lueften-mit-hilfe-des-smart-homes/
-- for formulas  see http://www.wettermail.de/wetter/feuchte.html

-- saturated vapor pressure in hPa
function getSaturationVaporPressure(temperature)
	if temperature >= 0 then
		return 6.1078 * math.exp( math.log(10) * ((7.5 * temperature) / (237.3 + temperature)) )
	else
		return 6.1078 * math.exp( math.log(10) * ((7.6 * temperature) / (240.7 + temperature)) )
	end
end

-- vapor pressure in hPa
function getVaporPressure(temperature, humidity)
	return humidity / 100 * getSaturationVaporPressure(temperature)
end

-- absolute humidity in g/m
function getAbsoluteHumidity(temperature, humidity)
	return math.exp(math.log(10) * 5) * 18.016 / 8314.3 * getVaporPressure(temperature, humidity) / (temperature + 273.15)
end



local temperature = readTemperature()
local humidity = readHumidity()

if temperature ~= nil and humidity ~= nil then
	local absolute = getAbsoluteHumidity(temperature, humidity)
	print("Temperature: "..string.format("%.2f",temperature).." C")
	print("Humidity rel.: "..string.format("%.2f",humidity).." %")
	print("Humidity abs.: "..string.format("%.2f",absolute).." g/m3")
end


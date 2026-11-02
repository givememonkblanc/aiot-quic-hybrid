"""Prepare the dedicated 4MiB OV2640 camera/AI OTA configuration."""
from pathlib import Path
import re

ROOT=Path(__file__).resolve().parents[2]
p=ROOT/'sdkconfig.mobility_ota'
source=p if p.exists() else ROOT/('sdkconfig.mobility' if (ROOT/'sdkconfig.mobility').exists() else 'sdkconfig')
s=source.read_text()
values={
 'CONFIG_HEAP_USE_HOOKS':None,
 'CONFIG_HEAP_POISONING_DISABLED':None,
 'CONFIG_HEAP_POISONING_LIGHT':'y',
 'CONFIG_FREERTOS_HZ':'1000',
 'CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160':None,
 'CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240':'y',
 'CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ':'240',
 'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME':'"partitions_mobility_ota.csv"',
 'CONFIG_PARTITION_TABLE_FILENAME':'"partitions_mobility_ota.csv"',
 'CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE':'y',
 'CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC':None,'CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC':'y',
 'CONFIG_COMPILER_OPTIMIZATION_DEBUG':None,'CONFIG_COMPILER_OPTIMIZATION_SIZE':None,
 'CONFIG_COMPILER_OPTIMIZATION_PERF':'y',
 'CONFIG_LOG_DEFAULT_LEVEL_INFO':None,'CONFIG_LOG_DEFAULT_LEVEL_WARN':'y',
 'CONFIG_LOG_DEFAULT_LEVEL':'2','CONFIG_LOG_MAXIMUM_LEVEL':'2',
 'CONFIG_MBEDTLS_CERTIFICATE_BUNDLE':None,'CONFIG_OV2640_SUPPORT':'y',
 'CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE':None,
 'CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_SILENT':'y',
 'CONFIG_MBEDTLS_TLS_SERVER_AND_CLIENT':None,'CONFIG_MBEDTLS_TLS_CLIENT_ONLY':'y',
 'CONFIG_MBEDTLS_ECP_FIXED_POINT_OPTIM':None,
 'CONFIG_MBEDTLS_ERROR_STRINGS':None,
}
for curve in ['SECP192R1','SECP224R1','SECP521R1','SECP192K1','SECP224K1','SECP256K1','BP256R1','BP384R1','BP512R1']:
 values[f'CONFIG_MBEDTLS_ECP_DP_{curve}_ENABLED']=None
for sensor in ['OV7670','OV7725','NT99141','OV3660','OV5640','GC2145','GC032A','GC0308','BF3005','BF20A6','SC030IOT','HM1055','HM0360','MEGA_CCM']:
 values[f'CONFIG_{sensor}_SUPPORT']=None
for key,value in values.items():
 replacement=f'{key}={value}' if value is not None else f'# {key} is not set'
 pattern=rf'^(?:{key}=.*|# {key} is not set)$'
 if re.search(pattern,s,re.M):s=re.sub(pattern,replacement,s,flags=re.M)
 else:s+='\n'+replacement+'\n'
if not p.exists() or p.read_text()!=s:p.write_text(s)

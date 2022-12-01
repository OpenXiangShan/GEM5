import argparse
from ast import parse
from asyncio import subprocess
from io import TextIOWrapper
from logging import warning
import random
import smtplib
from email.mime.text import MIMEText
from email.header import Header
import subprocess
from multiprocessing import Pool
import os
import time
import json
import utils
import psutil


#如何添加自定义的特殊变量
#在下面中找到Wstart,Wrun_multi,Wrun_single,Wend4个函数
#分别对应pre-work,multi模式work,single模式work,post-work
#在其中找到"#添加自定义特殊变量"注释,添加字典即可



parser = argparse.ArgumentParser(description='specify a cfg file')
parser.add_argument('-f', '--file', help='cfg file path')
args = parser.parse_args()

CFG_PATH = args.file


# check dir
cfgfile = utils.CFGReader(CFG_PATH)

works, pre_work, post_work = utils.getWorks(cfgfile)
if len(works) == 0:
    print('has no work to do')
    exit(1)



if not os.path.exists(cfgfile['global']['log_root']):
    os.makedirs(cfgfile['global']['log_root'])
#初始化tpoolId
utils.tpool_init(cfgfile)



def mailSendMsg(msg: str):
    if cfgfile['global']['debug_mode'] == 'true':
        print(msg)
    elif cfgfile['global']['debug_mode'] == 'false' and cfgfile['mail']['enable'] == 'true':
        try:
            smtp = smtplib.SMTP()
            smtp.connect(cfgfile['mail']['mail_host'])
            smtp.login(cfgfile['mail']['mail_sender'],
                       cfgfile['mail']['mail_license'])
            for receiver in cfgfile['mail']['mail_receivers'].split(';'):
                smtp.sendmail(cfgfile['mail']['mail_sender'], receiver, msg)
            smtp.quit()
        except Exception as e:
            print(msg)
            warning("mail send fail!!,check your license and network")


def startMain(work, log_dir: str, etcArg):
    time.sleep(random.random())
    task = work[1]
    log_ = log_dir
    #
    numaCores = cfgfile['work-'+work[0]].get('numacores')
    numa_args,C = utils.tpool_alloc(numaCores)
    #
    utils.argReplace(task, dict({'sublog': log_, 'numa': numa_args}, **etcArg))
    if not os.path.exists(log_):
        os.makedirs(log_)
    taskout = open(log_+'/taskout.txt', 'w')
    taskerr = open(log_+'/taskerr.txt', 'w')
    other = open(log_+'/other.txt', 'w')
    other.write('**********pre-task start**********\n')
    other.flush()
    startTime = time.time()
    # start pre-task
    pre = subprocess.run(args=task[0], shell=True, stdout=other,
                         stderr=subprocess.STDOUT, stdin=None, check=False, encoding='utf-8')
    # start task
    ret = None
    if pre.returncode == 0:
        ret = subprocess.run(args=task[1], shell=True, stdout=taskout,
                             stderr=taskerr, stdin=None, check=False, encoding='utf-8')
    taskout.close()
    taskerr.close()
    # start post-task
    post = None
    if ret and ret.returncode == 0:
        other.write('**********task finished,post-task start**********\n')
        other.flush()
        post = subprocess.run(args=task[2], shell=True, stdout=other,
                              stderr=subprocess.STDOUT, stdin=None, check=False, encoding='utf-8')
    tupTime = int(time.time()-startTime)  # 秒时间戳
    other.write(
        '**********runTime:{0}h:{1}m:{2}s**********\n'.format(tupTime // 3600, (tupTime % 3600)//60, (tupTime % 3600)%60))
    # start except-task
    if not (post and post.returncode == 0):
        other.write(
            '**********running error,except-task start**********\n')
        other.flush()
        exce = subprocess.run(args=task[3], shell=True, stdout=other,
                              stderr=subprocess.STDOUT, stdin=None, check=False, encoding='utf-8')
        other.close()
        mailSendMsg(
            """autotest find a error in:
            work:{0}
            log:{1}
            """.format(work[0],
                       log_
                       ))
        utils.tpool_free(C)
        return False
    other.close()
    utils.tpool_free(C)
    return True


def Wstart(log_dir, log_file: TextIOWrapper, etcArg: dict):
    '''
    执行pre-work
    '''
    task = pre_work.get('pre-work').copy()
    utils.argReplace(task, dict({'sublog': log_dir}, **etcArg))  # 添加自定义特殊变量
    log_file.write('**********pre-work:pre-task start**********\n')
    log_file.flush()
    # pre-work:pre-task
    pre = subprocess.run(args=task[0], shell=True, stdout=log_file,
                         stderr=subprocess.STDOUT, stdin=None, check=False, encoding='utf-8')
    # pre-work:task
    ret = None
    if pre.returncode == 0:
        log_file.write('**********pre-work:task start**********\n')
        log_file.flush()
        ret = subprocess.run(args=task[1], shell=True, stdout=log_file,
                             stderr=subprocess.STDOUT, stdin=None, check=False, encoding='utf-8')
    if not (ret and ret.returncode == 0):
        log_file.write(
            '**********pre-work: running error,except-task start**********\n')
        log_file.flush()
        exce = subprocess.run(args=task[3], shell=True, stdout=log_file,
                              stderr=subprocess.STDOUT, stdin=None, check=False, encoding='utf-8')
        return False

    return True


def Wrun_multi(log_dir, etcArg):
    pool = Pool(processes=int(cfgfile['iteration']['max_process']))
    results = []
    work_items = list(works.items())
    cnt = 0
    for work in work_items:
        random_int = random.randint(0, 10000)
        results.append(pool.apply_async(
            startMain,
            (work, log_dir+'/'+work[0],
             dict({'tid': cnt, 'random_int': random_int}, **etcArg))))#添加自定义特殊变量
        cnt += 1
        if cnt >= int(cfgfile['iteration']['max_process']):
            cnt = 0
        time.sleep(0.5)
    pool.close()
    pool.join()
    finished = True
    runErr_works = []
    for cnt in range(len(results)):
        if not results[cnt].get():
            runErr_works.append(work_items[cnt][0])
            finished = False
    return finished, runErr_works


def Wrun_single(log_dir, etcArg):
    pool = Pool(processes=int(cfgfile['iteration']['max_process']))
    results = []
    work_items = list(works.items())
    cnt = 0
    names = []
    for work in work_items:
        files, sublog = utils.get_file_list(
            cfgfile['work-'+work[0]]['binpath'])
        for i in range(len(files)):
            names.append(sublog[i])
            random_int = random.randint(0, 10000)
            results.append(
                pool.apply_async(
                    startMain,
                    (work,
                     log_dir+'/'+work[0]+'/'+sublog[i],
                     dict({'tid': cnt, 'random_int': random_int, 'binfile': files[i]}, **etcArg))))  # 添加自定义特殊变量
            cnt += 1
            if cnt >= int(cfgfile['iteration']['max_process']):
                cnt = 0
            time.sleep(0.5)
    pool.close()
    pool.join()
    finished = True
    runErr_works = []
    for cnt in range(len(results)):
        if not results[cnt].get():
            runErr_works.append(names[cnt])
            finished = False
    return finished, runErr_works

#发生任何错误,均会直接执行except-task
#返回post-work是否执行正确,如果前面任务执行错误,则直接执行post-work的except-task
def Wend(work_finished, log_dir, log_file: TextIOWrapper, etcArg: dict):
    '''
    执行post-work
    '''
    task = post_work.get('post-work').copy()
    utils.argReplace(task, dict({'sublog': log_dir}, **etcArg))  # 添加自定义特殊变量
    log_file.write('**********post-work:task start**********\n')
    log_file.flush()
    # post-work:task start
    ret = None 
    if work_finished:
        ret = subprocess.run(args=task[1], shell=True, stdout=log_file,
                            stderr=subprocess.STDOUT, stdin=None, check=False, encoding='utf-8')
    # post-work:post-task start
    post = None
    if ret and ret.returncode == 0:
        log_file.write('**********post-work:post-task start**********\n')
        log_file.flush()
        post = subprocess.run(args=task[2], shell=True, stdout=log_file,
                              stderr=subprocess.STDOUT, stdin=None, check=False, encoding='utf-8')
    log_file.close()
    if (not (post and post.returncode == 0)) or (not work_finished):
        subprocess.run(args=task[3], shell=True, stdout=None,
                       stderr=subprocess.STDOUT, stdin=None, check=False, encoding='utf-8')
        # 返回post-work的结果
        return False
    return True




def iteration():
    work_log_path = cfgfile['global']['log_root']
    if not os.path.exists(work_log_path):
        os.mkdir(work_log_path)
    work_log_file = open(work_log_path + '/iter_log.txt', 'w')
    etcArg = {}
    ignore_return = False
    error_msg = ''
    #######
    finished0 = Wstart(work_log_path, work_log_file, etcArg)
    finished1 = False
    if finished0:
        #######
        runErr_works=""
        if cfgfile['iteration']['working_mode'] == 'multi':
            finished1, runErr_works = Wrun_multi(work_log_path, etcArg)
        elif cfgfile['iteration']['working_mode'] == 'single':
            finished1, runErr_works = Wrun_single(work_log_path, etcArg)
        else:
            print("the error working_mode!")
            exit(1)
        if not finished1:
            error_msg += 'error works:{0}\n'.format(str(runErr_works))
    else:
        error_msg += 'pre-work running error\n'
    # 它会自动关闭work_log_file
    finished2 = Wend(finished1, work_log_path,
                        work_log_file, etcArg)
    if finished1 and (not finished2):#post-work执行发生错误
        error_msg += 'post-work running error\n'

    if not (finished0 and finished1 and finished2):  # 发生任何错误
        # 发送消息
        mailSendMsg(
            """autotest find a error in:
        error msg: 
        {0}
        """.format(error_msg))
        if cfgfile['iteration']['except_mode'] == 'stop':  # 如果是stop模式则直接退出
            return False
        elif cfgfile['iteration']['except_mode'] == 'ignore':  # 忽略,继续执行下一个测试
            ignore_return = True
    if ignore_return:
        return False
    return True


endless = int(cfgfile['iteration']['num']) < 0
iterations = int(cfgfile['iteration']['num'])
while endless or iterations > 0:

    finished = iteration()
    iterations -= 1

    if not finished:
        exit(-1)
    # 延迟
    time.sleep(eval(cfgfile['iteration']['end_delay']))
print('**********all tests finished**********')

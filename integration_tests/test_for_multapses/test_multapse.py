import pyNN.spiNNaker as p
from matplotlib import pylab
p.setup(timestep=1.0)
input_pop = p.Population(1, p.SpikeSourceArray, cellparams={"spike_times": [0]},
        label="input")
cell_params_lif = {'cm'        : 0.25, # nF
                     'i_offset'  : 0.0,
                     'tau_m'     : 20.0,
                     'tau_refrac': 2.0,
                     'tau_syn_E' : 5.0,
                     'tau_syn_I' : 5.0,
                     'v_reset'   : -70.0,
                     'v_rest'    : -65.0,
                     'v_thresh'  : -50.0
                     }
pop = p.Population(2, p.IF_curr_exp, cellparams=cell_params_lif, label="pop")

connections = list()
connections.append(p.Projection(input_pop, pop, p.AllToAllConnector(weights=[0.3, 1.0], delays=[1, 17])))
connections.append(p.Projection(input_pop, pop, p.AllToAllConnector(weights=[1.0, 0.7], delays=[2, 15])))
connections.append(p.Projection(input_pop, pop, p.AllToAllConnector(weights=[0.7, 0.3], delays=[3, 33])))

pre_weights = list()
pre_delays = list()
for connection in connections:
    pre_weights.append(connection.getWeights())
    pre_delays.append(connection.getDelays())

p.run(100)

post_weights = list()
post_delays = list()
for connection in connections:
    post_weights.append(connection.getWeights())
    post_delays.append(connection.getDelays())

for i in range(len(connections)):
    print "Weights before:", pre_weights[i], "and after:", post_weights[i]
    print "Delays before:", pre_delays[i], "and after:", post_delays[i]

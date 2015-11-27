from distutils.core import setup

setup(name='stir',
      version='3.0.0',
      url='https://github.com/UCL/STIR',
      description='STIR: Software for Tomographic Image Reconstruction',
      packages=['stir'],
      package_dir={'stir': '.'},
      package_data={'stir': ['*.*']})
